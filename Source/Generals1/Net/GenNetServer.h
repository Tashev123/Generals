#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Async/Future.h"
#include "Game/GenSkirmishSetup.h"
#include "Logic/GenMatch.h"
#include "Net/GenNet.h"
#include <atomic>

class FSocket;
class FRunnableThread;

/**
 * The host's server (see GenNet.h): the lobby, then the match's clock and its record. It works out nothing
 * of the game itself. Runs on a thread of its own, in its own program (-GenServer) or in a game.
 *
 * Who plays which side is settled by a seat token the server hands each player when it joins: a game that
 * comes back with its token gets its player back, with every order of the match so far to catch up with.
 * While a player is away the others play on; the simulation hears of it through a session order (the
 * player's allies may then command its army).
 */
class GENERALS1_API FGenNetServer : public FRunnable
{
public:
	FGenNetServer();
	virtual ~FGenNetServer() override;

	/** Listens on that port and begins; false (and Why) if that can't be done (the port is taken). */
	bool Start(int32 Port, FString* OutWhy = nullptr);
	/** Closes every connection and the thread. */
	void Shutdown();
	/** The match is over and its players have gone (or nobody has come for a long while): it can close. */
	bool IsDone() const { return bDone.load(); }
	int32 GetPort() const { return Port; }

	virtual uint32 Run() override;
	virtual void Stop() override { bStopping.store(true); }

private:
	struct FClient
	{
		TUniquePtr<FGenNetConnection> Conn;
		uint32 Id = 0;
		FString Name;
		uint64 Token = 0;
		bool bWelcomed = false;
		bool bLoaded = false;
		int32 Player = INDEX_NONE;    // the player it plays in the match
		double Since = 0.0;
		bool bGone = false;
	};
	enum class EPhase : uint8 { Lobby, Loading, Playing };

	void Accept();
	void Handle(FClient& C, GenNet::EMsg Type, const TArray<uint8>& Payload);
	void OnHello(FClient& C, const TArray<uint8>& Payload);
	void OnLobbyEdit(FClient& C, const TArray<uint8>& Payload);
	void OnStartRequest(FClient& C);
	void OnLeave(FClient& C);
	void TickClock();
	void NoteChecksums(const FClient& C, const TArray<uint8>& Payload);
	void Refuse(FClient& C, const FString& Why);
	void Notice(const FString& Text);
	void BroadcastLobby();
	void Broadcast(GenNet::EMsg Type, const TArray<uint8>& Payload, bool bInMatchOnly);
	void SendHistory(FClient& C);
	FClient* FindClient(uint32 Id);
	int32 SlotOf(uint32 ClientId) const;
	void SaveRecord();

	FSocket* Listener = nullptr;
	FRunnableThread* Thread = nullptr;
	int32 Port = 0;
	std::atomic<bool> bStopping{ false };
	std::atomic<bool> bDone{ false };

	TArray<FClient> Clients;
	uint32 NextClientId = 1;
	uint32 HostId = 0;
	FGenSetupData Lobby;
	uint32 SlotOwner[FGenSetupData::MaxSlots] = {};
	double LastActivity = 0.0;

	EPhase Phase = EPhase::Lobby;
	FGenMatchSettings Match;
	double PhaseSince = 0.0;
	TArray<uint64> SeatToken;           // by player: the token of the human who plays it (0: nobody's)
	TArray<bool> SeatConnected;
	TArray<FString> SeatName;
	TArray<FGenCommand> History;        // every order of the match, frame by frame
	TArray<FGenCommand> Incoming;       // since the last frame was closed
	uint32 NextFrame = 0;               // the next logic frame to close
	double ClockStart = 0.0;
	double FrameSeconds = 1.0 / 30.0;
	/** Checksums reported, by frame: (player, checksum). */
	TMap<uint32, TArray<TPair<int32, uint32>>> Reports;
	uint32 LastMismatchFrame = 0;
	bool bRecordSaved = false;

	// The port on the home router (GenUPnP), opened on a thread of its own at the start (not with -GenNoUPnP).
	TFuture<void> PortTask;
	FCriticalSection PortLock;
	FString PortNotice;               // where friends join, or why the port couldn't be opened
	bool bPortNoticeSent = false;
	std::atomic<bool> bPortMapped{ false };
};
