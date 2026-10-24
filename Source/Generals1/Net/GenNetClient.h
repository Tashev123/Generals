#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Game/GenSkirmishSetup.h"
#include "Logic/GenGameLogic.h"
#include "Net/GenNet.h"
#include <atomic>

class FRunnableThread;

/**
 * A player's game in a network game (see GenNet.h): talks to the host's server on a thread of its own. The
 * lobby, the start and the notices are handed to the game thread (Update and the Take* calls); the frames the
 * server closes go straight to the simulation, which asks for them as the order source (IGenOrderSource).
 *
 * A connection that breaks is made again, over and over for a while, with the seat token the server gave: in
 * a match the server sends what was missed and the game goes on where it was.
 */
class GENERALS1_API FGenNetClient : public FRunnable, public IGenOrderSource
{
public:
	enum class EState : uint8
	{
		Connecting,   // trying to reach the server (again)
		Lobby,        // in the game being set up
		Match,        // the match has been started
		Refused,      // the server said no (GetStatus says why)
		Failed,       // the server can't be reached
	};

	FGenNetClient();
	virtual ~FGenNetClient() override;

	/** Starts trying to reach host[:port] as that player (Token: a seat to come back to, 0: a new player). */
	void Begin(const FString& Address, const FString& Name, uint64 Token = 0);
	void Shutdown();

	// --- game thread ---
	/** Takes in what has come since the last call (every frame). */
	void Update();
	EState GetState() const { return State.load(); }
	/** A line for the screen: "connecting to ...", why the server refused, "connection lost - trying again". */
	FString GetStatus() const;
	bool HasLobby() const { return bHasLobby; }
	const FGenSetupData& GetLobby() const { return Lobby; }
	bool IsHost() const { return ClientId != 0 && ClientId == HostId; }
	/** The lobby slot this player sits in (INDEX_NONE: none). */
	int32 GetLocalSlot() const;
	bool IsSlotHuman(int32 Slot) const { return Slot >= 0 && Slot < FGenSetupData::MaxSlots && SlotOwners[Slot] != 0; }
	/** The setup as this player wants it: the server takes what this player may change. */
	void SendLobbyEdit(const FGenSetupData& Wanted);
	void SendName(const FString& Name);
	void SendChat(const FString& Text);
	/** (The host) the match begins. */
	void RequestStart();
	/** The match to start - the host has started it, or this game comes back to one - once: its settings, the
	 *  player this game plays, and whether it comes back (it has frames to catch up with). */
	bool TakeMatchStart(FGenMatchSettings& OutSettings, int32& OutPlayer, bool& bOutRejoin);
	/** The map is loaded: the match can begin. */
	void SendLoaded();
	/** Lines for the screen (joins, departures, mismatches) since the last call. */
	TArray<FString> TakeNotices();
	/** The first frame two games came apart at, as the server found (0: none). */
	uint32 GetMismatchFrame() const { return MismatchFrame; }
	double GetPingMs() const { return PingMs.load(); }
	uint64 GetToken() const { return Token.load(); }
	const FString& GetAddress() const { return Address; }

	// --- IGenOrderSource (any thread) ---
	virtual uint32 GetReadyFrames() const override { return ClosedFrames.load(std::memory_order_acquire); }
	virtual void TakeFrame(uint32 InFrame, TArray<FGenCommand>& Out) override;
	virtual void SendOrder(const FGenCommand& Command) override;
	virtual void NoteChecksum(uint32 InFrame, uint32 Checksum) override;

	virtual uint32 Run() override;
	virtual void Stop() override { bStopping.store(true); }

private:
	void Post(GenNet::EMsg Type, TArray<uint8>&& Payload);
	void OnMessage(GenNet::EMsg Type, const TArray<uint8>& Payload);
	void SetStatus(const FString& Text);

	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopping{ false };
	std::atomic<EState> State{ EState::Connecting };
	std::atomic<uint64> Token{ 0 };
	std::atomic<double> PingMs{ 0.0 };
	FString Address;
	FString Name;

	// Between the threads (Lock):
	mutable FCriticalSection Lock;
	FString Status;
	TArray<FGenCommand> Turns;                       // the orders of the frames closed, in order, from TurnHead on not yet taken
	int32 TurnHead = 0;
	std::atomic<uint32> ClosedFrames{ 0 };
	TArray<TPair<GenNet::EMsg, TArray<uint8>>> Outbox;   // to the server
	TArray<TPair<GenNet::EMsg, TArray<uint8>>> Inbox;    // for the game thread
	uint32 ChecksumFirst = 0;
	TArray<uint32> ChecksumQueue;

	// The network thread's:
	TUniquePtr<FGenNetConnection> Conn;
	double NextAttempt = 0.0;
	double FirstAttempt = 0.0;
	double LastPing = 0.0;
	bool bEverWelcomed = false;
	bool bInMatch = false;

	// The game thread's:
	FGenSetupData Lobby;
	uint32 SlotOwners[FGenSetupData::MaxSlots] = {};
	uint32 HostId = 0;
	uint32 ClientId = 0;
	bool bHasLobby = false;
	bool bStartPending = false;
	bool bStartIsRejoin = false;
	FGenMatchSettings StartSettings;
	int32 StartPlayer = INDEX_NONE;
	TArray<FString> Notices;
	uint32 MismatchFrame = 0;
};
