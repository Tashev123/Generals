#include "Net/GenNet.h"
#include "Generals1.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace
{
	/** No message is bigger than this (the whole history of a long match is a few megabytes). */
	constexpr uint32 MaxMessageBytes = 64u << 20;

	ISocketSubsystem* Sockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
}

const TCHAR* GenNet::MsgName(EMsg Msg)
{
	static const TCHAR* Names[] = { TEXT("None"), TEXT("Hello"), TEXT("Welcome"), TEXT("Refused"), TEXT("Lobby"), TEXT("LobbyEdit"), TEXT("Name"),
		TEXT("StartRequest"), TEXT("Start"), TEXT("Loaded"), TEXT("Order"), TEXT("Turn"), TEXT("History"), TEXT("Checksums"), TEXT("Mismatch"),
		TEXT("Chat"), TEXT("Notice"), TEXT("Ping"), TEXT("Pong") };
	static_assert(UE_ARRAY_COUNT(Names) == int32(EMsg::Count), "a name for every message");
	return uint8(Msg) < uint8(EMsg::Count) ? Names[uint8(Msg)] : TEXT("?");
}

void GenNet::SplitAddress(const FString& Address, FString& OutHost, int32& OutPort)
{
	OutHost = Address.TrimStartAndEnd();
	OutPort = DefaultPort;
	FString Host, Port;
	if (OutHost.Split(TEXT(":"), &Host, &Port, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && Port.IsNumeric())
	{
		OutHost = Host;
		OutPort = FCString::Atoi(*Port);
	}
}

// ---- the connection -------------------------------------------------------------------------------

FGenNetConnection::FGenNetConnection(FSocket* InSocket)
	: Socket(InSocket)
{
	if (Socket)
	{
		Socket->SetNonBlocking(true);
		Socket->SetNoDelay(true); // (orders are small and want to go at once)
		int32 Size = 0;
		Socket->SetSendBufferSize(1 << 20, Size);
		Socket->SetReceiveBufferSize(1 << 20, Size);
		TSharedRef<FInternetAddr> Peer = Sockets()->CreateInternetAddr();
		if (Socket->GetPeerAddress(*Peer))
		{
			Remote = Peer->ToString(true);
		}
	}
	bOpen = Socket != nullptr;
	LastReceive = FPlatformTime::Seconds();
}

FGenNetConnection::~FGenNetConnection()
{
	Close();
	if (Socket)
	{
		Sockets()->DestroySocket(Socket);
		Socket = nullptr;
	}
}

TUniquePtr<FGenNetConnection> FGenNetConnection::Connect(const FString& Host, int32 Port, double TimeoutSeconds, FString* OutWhy)
{
	ISocketSubsystem* SS = Sockets();
	if (!SS)
	{
		if (OutWhy) { *OutWhy = TEXT("no network"); }
		return nullptr;
	}
	// A name (a dynamic DNS name for a home connection whose address changes) or an address.
	FAddressInfoResult Found = SS->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Streaming);
	if (Found.ReturnCode != SE_NO_ERROR || Found.Results.Num() == 0)
	{
		if (OutWhy) { *OutWhy = FString::Printf(TEXT("can't find '%s'"), *Host); }
		return nullptr;
	}
	TSharedPtr<FInternetAddr> Addr = Found.Results[0].Address;
	for (const FAddressInfoResultData& R : Found.Results)
	{
		if (R.Address->GetProtocolType() == FNetworkProtocolTypes::IPv4)
		{
			Addr = R.Address; // (home connections are IPv4: the router forwards that)
			break;
		}
	}
	Addr->SetPort(Port);
	FSocket* Socket = SS->CreateSocket(NAME_Stream, TEXT("GenNet game"), Addr->GetProtocolType());
	if (!Socket)
	{
		if (OutWhy) { *OutWhy = TEXT("can't make a socket"); }
		return nullptr;
	}
	// A connection is tried without blocking and waited for, so that a server that isn't there doesn't hold
	// the attempt up for the system's twenty seconds.
	Socket->SetNonBlocking(true);
	Socket->Connect(*Addr);
	const double Until = FPlatformTime::Seconds() + TimeoutSeconds;
	ESocketConnectionState State = SCS_NotConnected;
	while (FPlatformTime::Seconds() < Until)
	{
		Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(50));
		State = Socket->GetConnectionState();
		if (State != SCS_NotConnected)
		{
			break;
		}
	}
	if (State != SCS_Connected)
	{
		if (OutWhy)
		{
			*OutWhy = FString::Printf(TEXT("no answer from %s"), *Addr->ToString(true));
		}
		SS->DestroySocket(Socket);
		return nullptr;
	}
	return MakeUnique<FGenNetConnection>(Socket);
}

void FGenNetConnection::Send(GenNet::EMsg Type, const TArray<uint8>& Payload)
{
	if (!bOpen)
	{
		return;
	}
	const uint32 Size = uint32(Payload.Num()) + 1;
	const uint8 Header[5] = { uint8(Size), uint8(Size >> 8), uint8(Size >> 16), uint8(Size >> 24), uint8(Type) };
	Out.Append(Header, 5);
	Out.Append(Payload);
}

bool FGenNetConnection::Pump()
{
	if (!bOpen || !Socket)
	{
		return false;
	}
	// Out, as far as the system takes it now.
	while (OutHead < Out.Num())
	{
		int32 Sent = 0;
		if (!Socket->Send(Out.GetData() + OutHead, Out.Num() - OutHead, Sent))
		{
			if (Sockets()->GetLastErrorCode() == SE_EWOULDBLOCK)
			{
				break;
			}
			bOpen = false;
			return false;
		}
		if (Sent <= 0)
		{
			break;
		}
		OutHead += Sent;
	}
	if (OutHead >= Out.Num())
	{
		Out.Reset();
		OutHead = 0;
	}
	else if (OutHead > (1 << 20))
	{
		Out.RemoveAt(0, OutHead, EAllowShrinking::No);
		OutHead = 0;
	}
	// In, whatever has come. (A stream socket's Recv fails when the other end has closed or the connection
	// broke, and succeeds with nothing when nothing has come.)
	uint8 Buffer[64 * 1024];
	for (int32 Guard = 0; Guard < 256; ++Guard)
	{
		int32 Read = 0;
		if (!Socket->Recv(Buffer, sizeof(Buffer), Read))
		{
			bOpen = false;
			return false;
		}
		if (Read <= 0)
		{
			break;
		}
		In.Append(Buffer, Read);
		LastReceive = FPlatformTime::Seconds();
		if (Read < int32(sizeof(Buffer)))
		{
			break;
		}
	}
	return true;
}

bool FGenNetConnection::Receive(GenNet::EMsg& OutType, TArray<uint8>& OutPayload)
{
	const int32 Have = In.Num() - InHead;
	if (Have < 5)
	{
		return false;
	}
	const uint8* P = In.GetData() + InHead;
	const uint32 Size = uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24);
	if (Size < 1 || Size > MaxMessageBytes || P[4] == 0 || P[4] >= uint8(GenNet::EMsg::Count))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Net: a bad message from %s (size %u, type %u): connection closed"), *Remote, Size, P[4]);
		bOpen = false;
		In.Reset();
		InHead = 0;
		return false;
	}
	if (uint32(Have) < 4 + Size)
	{
		return false;
	}
	OutType = GenNet::EMsg(P[4]);
	OutPayload.Reset();
	OutPayload.Append(P + 5, int32(Size) - 1);
	InHead += 4 + int32(Size);
	if (InHead >= In.Num())
	{
		In.Reset();
		InHead = 0;
	}
	else if (InHead > (1 << 20))
	{
		In.RemoveAt(0, InHead, EAllowShrinking::No);
		InHead = 0;
	}
	return true;
}

void FGenNetConnection::Close()
{
	if (Socket && bOpen)
	{
		Pump(); // (what is queued, as far as it goes)
		Socket->Close();
	}
	bOpen = false;
}

double FGenNetConnection::GetSilence() const
{
	return FPlatformTime::Seconds() - LastReceive;
}
