#pragma once

#include "CoreMinimal.h"

class FSocket;

/**
 * Network games: one program is the host's server, every player's game connects to it over TCP.
 *
 * The games don't send each other what happens - each works out the whole game itself, from the same
 * settings and the same orders (lockstep, as the original). The server is the clock: thirty times a second
 * it closes a logic frame, stamps the orders that came in meanwhile with it and sends them to everybody, and
 * a game works out a frame only once it has that frame's orders. It keeps every order of the match, so a
 * player who drops out can come back: the game gets the orders so far and works its way up to the present.
 */
namespace GenNet
{
	/** Changes whenever the messages change: a game and a server of different versions don't talk. */
	constexpr uint32 ProtocolVersion = 1;
	constexpr int32 DefaultPort = 8088;

	enum class EMsg : uint8
	{
		None,
		Hello,          // game -> server: protocol, build, data, name, seat token (0: a new player)
		Welcome,        // server -> game: its id, its token, whether it is the host, whether it comes back to a match
		Refused,        // server -> game: why not (and the server closes the connection)
		Lobby,          // server -> games: the game being set up, who sits where
		LobbyEdit,      // game -> server: the setup as this player wants it (the server takes what it may change)
		Name,           // game -> server: the player's name
		StartRequest,   // host's game -> server: start the match
		Start,          // server -> game: the match's settings and which player this game plays
		Loaded,         // game -> server: the map is loaded, the match can begin
		Order,          // game -> server: an order of its player
		Turn,           // server -> games: a logic frame is closed: its orders
		History,        // server -> a game that comes back: the settings, its player and every order so far
		Checksums,      // game -> server: what its simulation came to after some frames
		Mismatch,       // server -> games: two games came to different checksums at a frame
		Chat,           // both ways: a line of text
		Notice,         // server -> games: somebody joined, left, dropped out, came back
		Ping,
		Pong,
		Count
	};

	const TCHAR* MsgName(EMsg Msg);

	/** "host[:port]" -> host, port (the default when there is none). */
	void SplitAddress(const FString& Address, FString& OutHost, int32& OutPort);
}

/**
 * One TCP connection, carrying whole messages: [size: uint32][type: uint8][payload]. Non-blocking: Pump sends
 * what it can of what is queued and reads what has come in, and is called over and over by the thread that owns
 * the connection.
 */
class GENERALS1_API FGenNetConnection
{
public:
	/** Takes the socket over (it is closed and destroyed with the connection). */
	explicit FGenNetConnection(FSocket* InSocket);
	~FGenNetConnection();
	FGenNetConnection(const FGenNetConnection&) = delete;
	FGenNetConnection& operator=(const FGenNetConnection&) = delete;

	/** Opens a connection to host:port (a name or an address); null and Why if it can't be made. */
	static TUniquePtr<FGenNetConnection> Connect(const FString& Host, int32 Port, double TimeoutSeconds, FString* OutWhy);

	void Send(GenNet::EMsg Type, const TArray<uint8>& Payload);
	void Send(GenNet::EMsg Type) { Send(Type, TArray<uint8>()); }
	/** Sends and receives what it can; false once the connection is gone (closed, broken, or a bad message). */
	bool Pump();
	/** The next whole message that has come in, if any. */
	bool Receive(GenNet::EMsg& OutType, TArray<uint8>& OutPayload);
	/** Closes it (what is queued is sent first, as far as it goes at once). */
	void Close();
	bool IsOpen() const { return bOpen; }
	/** Seconds since something last came in. */
	double GetSilence() const;
	int32 GetQueuedBytes() const { return Out.Num() - OutHead; }
	const FString& GetRemote() const { return Remote; }

private:
	FSocket* Socket = nullptr;
	FString Remote;
	TArray<uint8> Out;
	int32 OutHead = 0;
	TArray<uint8> In;
	int32 InHead = 0;
	bool bOpen = true;
	double LastReceive = 0.0;
};
