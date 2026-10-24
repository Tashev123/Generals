#include "Net/GenUPnP.h"
#include "Generals1.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace
{
	struct FUrl
	{
		FString Host;
		int32 Port = 80;
		FString Path = TEXT("/");
	};

	/** The router's port service, once found (for ClosePort). */
	FCriticalSection RouterLock;
	FUrl ControlUrl;
	FString ServiceType;
	FString MappedLocalIP;

	ISocketSubsystem* Sockets()
	{
		return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	bool ParseUrl(const FString& Url, FUrl& Out)
	{
		FString Rest = Url.TrimStartAndEnd();
		if (!Rest.RemoveFromStart(TEXT("http://"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		int32 Slash = INDEX_NONE;
		FString HostPort = Rest;
		Out.Path = TEXT("/");
		if (Rest.FindChar(TEXT('/'), Slash))
		{
			HostPort = Rest.Left(Slash);
			Out.Path = Rest.Mid(Slash);
		}
		FString H, P;
		if (HostPort.Split(TEXT(":"), &H, &P))
		{
			Out.Host = H;
			Out.Port = FCString::Atoi(*P);
		}
		else
		{
			Out.Host = HostPort;
			Out.Port = 80;
		}
		return !Out.Host.IsEmpty();
	}

	/** The text between Open and Close (after From), or empty. */
	FString Between(const FString& Text, const FString& Open, const FString& Close, int32 From = 0, int32* OutEnd = nullptr)
	{
		const int32 A = Text.Find(Open, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
		if (A == INDEX_NONE)
		{
			return FString();
		}
		const int32 B = Text.Find(Close, ESearchCase::IgnoreCase, ESearchDir::FromStart, A + Open.Len());
		if (B == INDEX_NONE)
		{
			return FString();
		}
		if (OutEnd) { *OutEnd = B + Close.Len(); }
		return Text.Mid(A + Open.Len(), B - A - Open.Len()).TrimStartAndEnd();
	}

	/** One HTTP request on a connection of its own; the answer's status and body. */
	bool HttpRequest(const FUrl& Url, const TCHAR* Method, const FString& ExtraHeaders, const FString& Body, int32& OutStatus, FString& OutBody, FString* OutLocalIP = nullptr)
	{
		ISocketSubsystem* SS = Sockets();
		TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
		bool bValid = false;
		Addr->SetIp(*Url.Host, bValid);
		if (!bValid)
		{
			return false;
		}
		Addr->SetPort(Url.Port);
		FSocket* S = SS->CreateSocket(NAME_Stream, TEXT("UPnP"), Addr->GetProtocolType());
		if (!S)
		{
			return false;
		}
		ON_SCOPE_EXIT { SS->DestroySocket(S); };
		if (!S->Connect(*Addr))
		{
			return false;
		}
		if (OutLocalIP)
		{
			TSharedRef<FInternetAddr> Local = SS->CreateInternetAddr();
			S->GetAddress(*Local);
			*OutLocalIP = Local->ToString(false);
		}
		const FTCHARToUTF8 BodyUtf8(*Body);
		const FString Head = FString::Printf(TEXT("%s %s HTTP/1.1\r\nHost: %s:%d\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n"), Method, *Url.Path, *Url.Host,
			Url.Port, *ExtraHeaders, BodyUtf8.Length());
		const FTCHARToUTF8 HeadUtf8(*Head);
		TArray<uint8> Out;
		Out.Append(reinterpret_cast<const uint8*>(HeadUtf8.Get()), HeadUtf8.Length());
		Out.Append(reinterpret_cast<const uint8*>(BodyUtf8.Get()), BodyUtf8.Length());
		int32 Sent = 0;
		for (int32 Done = 0; Done < Out.Num(); Done += Sent)
		{
			if (!S->Send(Out.GetData() + Done, Out.Num() - Done, Sent) || Sent <= 0)
			{
				return false;
			}
		}
		// The answer, until the router closes the connection (or for five seconds).
		TArray<uint8> In;
		uint8 Buffer[8192];
		const double Until = FPlatformTime::Seconds() + 5.0;
		while (FPlatformTime::Seconds() < Until)
		{
			if (!S->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
			{
				continue;
			}
			int32 Read = 0;
			if (!S->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0)
			{
				break;
			}
			In.Append(Buffer, Read);
		}
		In.Add(0);
		const FString Text = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(In.GetData()));
		FString Headers, Content;
		if (!Text.Split(TEXT("\r\n\r\n"), &Headers, &Content))
		{
			return false;
		}
		FString StatusLine = Headers;
		Headers.Split(TEXT("\r\n"), &StatusLine, nullptr);
		TArray<FString> StatusParts;
		StatusLine.ParseIntoArrayWS(StatusParts);
		OutStatus = StatusParts.Num() >= 2 ? FCString::Atoi(*StatusParts[1]) : 0;
		if (Headers.Contains(TEXT("chunked")))
		{
			// (Transfer-Encoding: chunked - the size of each piece in hex on a line of its own before it.)
			FString Joined, Rest = Content;
			for (;;)
			{
				FString SizeLine, After;
				if (!Rest.Split(TEXT("\r\n"), &SizeLine, &After))
				{
					break;
				}
				const int32 Size = FParse::HexNumber(*SizeLine);
				if (Size <= 0 || After.Len() < Size)
				{
					break;
				}
				Joined += After.Left(Size);
				Rest = After.Mid(Size + 2);
			}
			Content = Joined;
		}
		OutBody = Content;
		return OutStatus > 0;
	}

	bool Soap(const FString& Action, const FString& Args, FString& OutBody, FString* OutLocalIP = nullptr)
	{
		const FString Body = FString::Printf(TEXT("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ")
			TEXT("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n"),
			*Action, *ServiceType, *Args, *Action);
		const FString Headers = FString::Printf(TEXT("Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"), *ServiceType, *Action);
		int32 Status = 0;
		return HttpRequest(ControlUrl, TEXT("POST"), Headers, Body, Status, OutBody, OutLocalIP) && Status == 200;
	}

	/** SSDP: who on the local network is an internet gateway - the address of its description. */
	FString DiscoverRouter()
	{
		ISocketSubsystem* SS = Sockets();
		FSocket* U = SS->CreateSocket(NAME_DGram, TEXT("SSDP"), FNetworkProtocolTypes::IPv4);
		if (!U)
		{
			return FString();
		}
		ON_SCOPE_EXIT { SS->DestroySocket(U); };
		TSharedRef<FInternetAddr> Any = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Any->SetAnyAddress();
		Any->SetPort(0);
		U->Bind(*Any);
		TSharedRef<FInternetAddr> Group = SS->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		bool bValid = false;
		Group->SetIp(TEXT("239.255.255.250"), bValid);
		Group->SetPort(1900);
		for (const TCHAR* Search : { TEXT("urn:schemas-upnp-org:device:InternetGatewayDevice:1"), TEXT("urn:schemas-upnp-org:device:InternetGatewayDevice:2") })
		{
			const FString Msg = FString::Printf(TEXT("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n"), Search);
			const FTCHARToUTF8 Bytes(*Msg);
			int32 Sent = 0;
			U->SendTo(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length(), Sent, *Group);
		}
		const double Until = FPlatformTime::Seconds() + 3.0;
		while (FPlatformTime::Seconds() < Until)
		{
			if (!U->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200)))
			{
				continue;
			}
			uint8 Buffer[2048];
			int32 Read = 0;
			TSharedRef<FInternetAddr> From = SS->CreateInternetAddr();
			if (!U->RecvFrom(Buffer, sizeof(Buffer) - 1, Read, *From) || Read <= 0)
			{
				continue;
			}
			Buffer[Read] = 0;
			const FString Answer = UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(Buffer));
			TArray<FString> Lines;
			Answer.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				if (Line.StartsWith(TEXT("LOCATION:"), ESearchCase::IgnoreCase))
				{
					return Line.Mid(9).TrimStartAndEnd();
				}
			}
		}
		return FString();
	}
}

GenUPnP::FResult GenUPnP::OpenPort(int32 Port, const FString& Description)
{
	FScopeLock L(&RouterLock);
	FResult R;
	const FString Location = DiscoverRouter();
	FUrl DescUrl;
	if (Location.IsEmpty() || !ParseUrl(Location, DescUrl))
	{
		R.Error = TEXT("no router answered (UPnP is off on it, or there is none)");
		return R;
	}
	int32 Status = 0;
	FString Desc;
	if (!HttpRequest(DescUrl, TEXT("GET"), FString(), FString(), Status, Desc) || Status != 200)
	{
		R.Error = FString::Printf(TEXT("the router's description can't be read (%s)"), *Location);
		return R;
	}
	R.bFoundRouter = true;
	R.Router = FString::Printf(TEXT("%s %s"), *Between(Desc, TEXT("<manufacturer>"), TEXT("</manufacturer>")), *Between(Desc, TEXT("<modelName>"), TEXT("</modelName>"))).TrimStartAndEnd();
	// The service that forwards ports: WANIPConnection (or, on a PPPoE line, WANPPPConnection).
	FString Control;
	ServiceType.Reset();
	for (const TCHAR* Wanted : { TEXT("WANIPConnection:2"), TEXT("WANIPConnection:1"), TEXT("WANPPPConnection:1") })
	{
		int32 From = 0;
		for (;;)
		{
			int32 End = 0;
			const FString Service = Between(Desc, TEXT("<service>"), TEXT("</service>"), From, &End);
			if (Service.IsEmpty())
			{
				break;
			}
			From = End;
			const FString Type = Between(Service, TEXT("<serviceType>"), TEXT("</serviceType>"));
			if (Type.EndsWith(Wanted))
			{
				ServiceType = Type;
				Control = Between(Service, TEXT("<controlURL>"), TEXT("</controlURL>"));
				break;
			}
		}
		if (!ServiceType.IsEmpty())
		{
			break;
		}
	}
	if (ServiceType.IsEmpty() || Control.IsEmpty())
	{
		R.Error = FString::Printf(TEXT("%s doesn't forward ports by UPnP"), *R.Router);
		return R;
	}
	if (Control.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
	{
		ParseUrl(Control, ControlUrl);
	}
	else
	{
		ControlUrl = DescUrl;
		ControlUrl.Path = Control.StartsWith(TEXT("/")) ? Control : TEXT("/") + Control;
	}
	// The public address, and the address of this computer on the home network (the one the router is reached by).
	FString Answer;
	if (Soap(TEXT("GetExternalIPAddress"), FString(), Answer, &R.LocalIP))
	{
		R.ExternalIP = Between(Answer, TEXT("<NewExternalIPAddress>"), TEXT("</NewExternalIPAddress>"));
	}
	// The forwarding itself: for good (a lease of 0), or for a week where the router wants a limit.
	for (const int32 Lease : { 0, 604800 })
	{
		const FString Args = FString::Printf(TEXT("<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>")
			TEXT("<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>")
			TEXT("<NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>"), Port, Port, *R.LocalIP, *Description, Lease);
		if (Soap(TEXT("AddPortMapping"), Args, Answer))
		{
			R.bPortOpen = true;
			MappedLocalIP = R.LocalIP;
			break;
		}
		R.Error = FString::Printf(TEXT("%s refused to forward the port: %s %s"), *R.Router, *Between(Answer, TEXT("<errorCode>"), TEXT("</errorCode>")),
			*Between(Answer, TEXT("<errorDescription>"), TEXT("</errorDescription>")));
	}
	if (R.bPortOpen)
	{
		R.Error.Reset();
	}
	return R;
}

void GenUPnP::ClosePort(int32 Port)
{
	FScopeLock L(&RouterLock);
	if (ServiceType.IsEmpty() || MappedLocalIP.IsEmpty())
	{
		return;
	}
	FString Answer;
	Soap(TEXT("DeletePortMapping"), FString::Printf(TEXT("<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>"), Port), Answer);
	MappedLocalIP.Reset();
}
