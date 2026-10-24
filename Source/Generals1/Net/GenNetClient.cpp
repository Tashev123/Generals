#include "Net/GenNetClient.h"
#include "Generals1.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

using GenNet::EMsg;

namespace
{
	/** Silence from a server that pings back twice a second: the connection is gone. */
	constexpr double SilenceLimit = 8.0;
	/** How long the server is tried for: a first connection, after a lobby, during a match (a home line that
	 *  went down and comes back with a new address takes a few minutes). */
	constexpr double FirstTryLimit = 90.0;
	constexpr double LobbyTryLimit = 120.0;
	constexpr double MatchTryLimit = 15.0 * 60.0;

	FString ReadText(const TArray<uint8>& Payload)
	{
		FMemoryReader R(Payload);
		FString Text;
		R << Text;
		return Text;
	}
}

FGenNetClient::FGenNetClient() = default;

FGenNetClient::~FGenNetClient()
{
	Shutdown();
}

void FGenNetClient::Begin(const FString& InAddress, const FString& InName, uint64 InToken)
{
	Address = InAddress;
	Name = InName;
	Token.store(InToken);
	SetStatus(FString::Printf(TEXT("Connecting to %s"), *Address));
	Thread = FRunnableThread::Create(this, TEXT("GenNetClient"), 0, TPri_AboveNormal);
}

void FGenNetClient::Shutdown()
{
	if (Thread)
	{
		bStopping.store(true);
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

void FGenNetClient::SetStatus(const FString& Text)
{
	FScopeLock L(&Lock);
	Status = Text;
}

FString FGenNetClient::GetStatus() const
{
	FScopeLock L(&Lock);
	return Status;
}

// ---- the network thread ---------------------------------------------------------------------------

uint32 FGenNetClient::Run()
{
	FString Host;
	int32 Port = GenNet::DefaultPort;
	GenNet::SplitAddress(Address, Host, Port);
	FirstAttempt = FPlatformTime::Seconds();
	TArray<uint8> Payload;
	while (!bStopping.load())
	{
		const double Now = FPlatformTime::Seconds();
		if (!Conn)
		{
			const EState S = State.load();
			if (S == EState::Refused || S == EState::Failed || Now < NextAttempt)
			{
				FPlatformProcess::SleepNoStats(0.02f);
				continue;
			}
			const double Limit = bInMatch ? MatchTryLimit : (bEverWelcomed ? LobbyTryLimit : FirstTryLimit);
			if (Now - FirstAttempt > Limit)
			{
				State.store(EState::Failed);
				SetStatus(FString::Printf(TEXT("%s can't be reached"), *Address));
				UE_LOG(LogGenerals, Warning, TEXT("Net: %s can't be reached - giving up"), *Address);
				continue;
			}
			FString Why;
			Conn = FGenNetConnection::Connect(Host, Port, 3.0, &Why);
			if (!Conn)
			{
				SetStatus(FString::Printf(TEXT("%s %s: %s - trying again"), bInMatch ? TEXT("Lost the host,") : TEXT("Connecting to"), *Address, *Why));
				NextAttempt = Now + 2.0;
				continue;
			}
			// Who this is: the program and data it plays with, the name, and the seat it may come back to.
			TArray<uint8> Hello;
			FMemoryWriter W(Hello);
			uint32 Protocol = GenNet::ProtocolVersion, DataHash = GenBuild::GetDataHash();
			FString Build = GenBuild::GetBuildId(), PlayerName = Name;
			uint64 Seat = Token.load();
			W << Protocol << Build << DataHash << PlayerName << Seat;
			Conn->Send(EMsg::Hello, Hello);
			LastPing = Now;
			SetStatus(FString::Printf(TEXT("Connected to %s"), *Address));
			UE_LOG(LogGenerals, Log, TEXT("Net: connected to %s (%s)%s"), *Address, *Conn->GetRemote(), Seat ? TEXT(", coming back to a seat") : TEXT(""));
		}

		// What the game has for the server: orders, lobby wishes, and the checksums a few frames at a time.
		{
			FScopeLock L(&Lock);
			for (TPair<EMsg, TArray<uint8>>& M : Outbox)
			{
				Conn->Send(M.Key, M.Value);
			}
			Outbox.Reset();
			if (ChecksumQueue.Num() >= 10)
			{
				TArray<uint8> Sums;
				FMemoryWriter W(Sums);
				uint32 First = ChecksumFirst;
				int32 Num = ChecksumQueue.Num();
				W << First << Num;
				for (uint32& Crc : ChecksumQueue)
				{
					W << Crc;
				}
				Conn->Send(EMsg::Checksums, Sums);
				ChecksumFirst += uint32(Num);
				ChecksumQueue.Reset();
			}
		}
		if (Now - LastPing > 0.5)
		{
			TArray<uint8> Ping;
			FMemoryWriter W(Ping);
			double T = Now;
			W << T;
			Conn->Send(EMsg::Ping, Ping);
			LastPing = Now;
		}
		const bool bOk = Conn->Pump();
		EMsg Type;
		while (Conn->Receive(Type, Payload))
		{
			OnMessage(Type, Payload);
		}
		if (!bOk || !Conn->IsOpen() || Conn->GetSilence() > SilenceLimit)
		{
			Conn.Reset();
			if (State.load() != EState::Refused)
			{
				UE_LOG(LogGenerals, Warning, TEXT("Net: the connection to %s is lost - trying again"), *Address);
				SetStatus(FString::Printf(TEXT("Lost the connection to %s - trying again"), *Address));
				if (!bInMatch)
				{
					State.store(EState::Connecting);
				}
				NextAttempt = Now + 1.0;
				FirstAttempt = Now;
			}
			continue;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	if (Conn)
	{
		Conn->Close();
		Conn.Reset();
	}
	return 0;
}

void FGenNetClient::OnMessage(EMsg Type, const TArray<uint8>& Payload)
{
	FMemoryReader R(Payload);
	switch (Type)
	{
	case EMsg::Welcome:
	{
		uint32 Id = 0;
		uint64 Seat = 0;
		uint8 bHost = 0, bRejoin = 0;
		R << Id << Seat << bHost << bRejoin;
		Token.store(Seat);
		bEverWelcomed = true;
		if (!bInMatch && !bRejoin)
		{
			State.store(EState::Lobby);
		}
		SetStatus(FString::Printf(TEXT("Connected to %s"), *Address));
		break;
	}
	case EMsg::Refused:
		State.store(EState::Refused);
		SetStatus(FString::Printf(TEXT("The host says no: %s"), *ReadText(Payload)));
		UE_LOG(LogGenerals, Warning, TEXT("Net: refused: %s"), *ReadText(Payload));
		break;
	case EMsg::Start:
	{
		FScopeLock L(&Lock);
		bInMatch = true;
		State.store(EState::Match);
		Turns.Reset();
		TurnHead = 0;
		ClosedFrames.store(0, std::memory_order_release);
		ChecksumQueue.Reset();
		ChecksumFirst = 1;
		break;
	}
	case EMsg::Turn:
	{
		uint32 FrameNo = 0;
		int32 Num = 0;
		R << FrameNo << Num;
		TArray<FGenCommand> Orders;
		Orders.SetNum(FMath::Clamp(Num, 0, 65536));
		for (FGenCommand& C : Orders)
		{
			C.Serialize(R);
		}
		FScopeLock L(&Lock);
		const uint32 Closed = ClosedFrames.load();
		if (FrameNo == Closed)
		{
			Turns.Append(MoveTemp(Orders));
			ClosedFrames.store(Closed + 1, std::memory_order_release);
		}
		else if (FrameNo > Closed)
		{
			UE_LOG(LogGenerals, Error, TEXT("Net: frame %u came after frame %u - frames missing"), FrameNo, Closed);
		}
		return; // (the simulation's, not the game thread's)
	}
	case EMsg::History:
	{
		// Every order so far. Coming back to a match this game is still playing: what it missed. Coming back
		// to one it isn't playing (it was closed): the match is started from these.
		FGenMatchSettings Settings;
		Settings.Serialize(R);
		int32 Player = INDEX_NONE;
		uint32 Closed = 0;
		int32 Num = 0;
		R << Player << Closed << Num;
		TArray<FGenCommand> Orders;
		Orders.SetNum(FMath::Clamp(Num, 0, 4 << 20));
		for (FGenCommand& C : Orders)
		{
			C.Serialize(R);
		}
		const bool bFresh = !bInMatch;
		{
			FScopeLock L(&Lock);
			if (bFresh)
			{
				Turns = MoveTemp(Orders);
				TurnHead = 0;
				ClosedFrames.store(Closed, std::memory_order_release);
				ChecksumQueue.Reset();
				ChecksumFirst = 1;
			}
			else
			{
				const uint32 Have = ClosedFrames.load();
				for (FGenCommand& C : Orders)
				{
					if (C.Frame >= Have)
					{
						Turns.Add(MoveTemp(C));
					}
				}
				ClosedFrames.store(FMath::Max(Have, Closed), std::memory_order_release);
			}
		}
		bInMatch = true;
		State.store(EState::Match);
		UE_LOG(LogGenerals, Log, TEXT("Net: back in the match as player %d: %u frames, %d orders%s"), Player, Closed, Num, bFresh ? TEXT(" (from the start)") : TEXT(""));
		if (!bFresh)
		{
			return;
		}
		break;
	}
	case EMsg::Pong:
	{
		double T = 0.0;
		R << T;
		PingMs.store((FPlatformTime::Seconds() - T) * 1000.0);
		return;
	}
	default:
		break;
	}
	// The rest is the game thread's.
	FScopeLock L(&Lock);
	Inbox.Emplace(Type, Payload);
}

// ---- the game thread ------------------------------------------------------------------------------

void FGenNetClient::Update()
{
	TArray<TPair<EMsg, TArray<uint8>>> Messages;
	{
		FScopeLock L(&Lock);
		Messages = MoveTemp(Inbox);
		Inbox.Reset();
	}
	for (const TPair<EMsg, TArray<uint8>>& M : Messages)
	{
		FMemoryReader R(M.Value);
		switch (M.Key)
		{
		case EMsg::Welcome:
		{
			uint32 Id = 0;
			R << Id;
			ClientId = Id;
			break;
		}
		case EMsg::Lobby:
			Lobby.Serialize(R);
			for (uint32& Owner : SlotOwners)
			{
				R << Owner;
			}
			R << HostId;
			bHasLobby = !R.IsError();
			break;
		case EMsg::Start:
			StartSettings.Serialize(R);
			R << StartPlayer;
			bStartPending = !R.IsError();
			bStartIsRejoin = false;
			UE_LOG(LogGenerals, Log, TEXT("Net: the host starts the match; this game plays player %d"), StartPlayer);
			break;
		case EMsg::History:
			StartSettings.Serialize(R);
			R << StartPlayer;
			bStartPending = !R.IsError();
			bStartIsRejoin = true;
			break;
		case EMsg::Refused:
		case EMsg::Notice:
		case EMsg::Chat:
			Notices.Add(ReadText(M.Value));
			break;
		case EMsg::Mismatch:
		{
			uint32 FrameNo = 0;
			FString Detail;
			R << FrameNo << Detail;
			UE_LOG(LogGenerals, Warning, TEXT("Net: MISMATCH at frame %u: %s"), FrameNo, *Detail);
			if (MismatchFrame == 0)
			{
				MismatchFrame = FrameNo;
				Notices.Add(FString::Printf(TEXT("The games have come apart at frame %u (%s)"), FrameNo, *Detail));
			}
			break;
		}
		default:
			break;
		}
	}
}

int32 FGenNetClient::GetLocalSlot() const
{
	for (int32 s = 0; s < FGenSetupData::MaxSlots; ++s)
	{
		if (ClientId != 0 && SlotOwners[s] == ClientId) { return s; }
	}
	return INDEX_NONE;
}

void FGenNetClient::Post(EMsg Type, TArray<uint8>&& Payload)
{
	FScopeLock L(&Lock);
	Outbox.Emplace(Type, MoveTemp(Payload));
}

void FGenNetClient::SendLobbyEdit(const FGenSetupData& Wanted)
{
	TArray<uint8> Bytes;
	FMemoryWriter W(Bytes);
	FGenSetupData Copy = Wanted;
	Copy.Serialize(W);
	Post(EMsg::LobbyEdit, MoveTemp(Bytes));
}

void FGenNetClient::SendName(const FString& InName)
{
	Name = InName;
	TArray<uint8> Bytes;
	FMemoryWriter W(Bytes);
	FString N = InName;
	W << N;
	Post(EMsg::Name, MoveTemp(Bytes));
}

void FGenNetClient::SendChat(const FString& Text)
{
	TArray<uint8> Bytes;
	FMemoryWriter W(Bytes);
	FString T = Text;
	W << T;
	Post(EMsg::Chat, MoveTemp(Bytes));
}

void FGenNetClient::RequestStart()
{
	Post(EMsg::StartRequest, TArray<uint8>());
}

bool FGenNetClient::TakeMatchStart(FGenMatchSettings& OutSettings, int32& OutPlayer, bool& bOutRejoin)
{
	if (!bStartPending)
	{
		return false;
	}
	bStartPending = false;
	OutSettings = StartSettings;
	OutPlayer = StartPlayer;
	bOutRejoin = bStartIsRejoin;
	return true;
}

void FGenNetClient::SendLoaded()
{
	Post(EMsg::Loaded, TArray<uint8>());
}

TArray<FString> FGenNetClient::TakeNotices()
{
	TArray<FString> Out = MoveTemp(Notices);
	Notices.Reset();
	return Out;
}

// ---- the simulation's side ------------------------------------------------------------------------

void FGenNetClient::TakeFrame(uint32 InFrame, TArray<FGenCommand>& Out)
{
	FScopeLock L(&Lock);
	while (TurnHead < Turns.Num() && Turns[TurnHead].Frame <= InFrame)
	{
		if (Turns[TurnHead].Frame == InFrame)
		{
			Out.Add(MoveTemp(Turns[TurnHead]));
		}
		++TurnHead;
	}
	// (Taken orders go now and then, not one frame at a time: the history of a long match is many of them.)
	if (TurnHead > 1024 && TurnHead * 2 > Turns.Num())
	{
		Turns.RemoveAt(0, TurnHead, EAllowShrinking::No);
		TurnHead = 0;
	}
}

void FGenNetClient::SendOrder(const FGenCommand& Command)
{
	TArray<uint8> Bytes;
	FMemoryWriter W(Bytes);
	FGenCommand Copy = Command;
	Copy.Serialize(W);
	Post(EMsg::Order, MoveTemp(Bytes));
}

void FGenNetClient::NoteChecksum(uint32 InFrame, uint32 Checksum)
{
	FScopeLock L(&Lock);
	if (ChecksumQueue.Num() == 0 || InFrame != ChecksumFirst + uint32(ChecksumQueue.Num()))
	{
		ChecksumQueue.Reset();
		ChecksumFirst = InFrame;
	}
	ChecksumQueue.Add(Checksum);
}
