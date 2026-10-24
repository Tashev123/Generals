#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HAL/PlatformProcess.h"
#include "Net/GenNetClient.h"
#include "Net/GenNetServer.h"
#include "GenNetSubsystem.generated.h"

/**
 * This program's part in network games: the connection to the host's server (and, for the program that is
 * the server, the server). It lives with the game instance, so the connection outlasts the level being
 * loaded again between the lobby and the match.
 */
UCLASS()
class GENERALS1_API UGenNetSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static UGenNetSubsystem* Get(const UObject* WorldContext);
	virtual void Deinitialize() override;

	/**
	 * Hosts a game: starts the server as a program of its own in the background (so that the match goes on
	 * should this game close or crash - it can come back) and joins it. False and Why if it can't be started.
	 */
	bool Host(const FString& PlayerName, int32 Port, FString* OutWhy = nullptr);
	/** Joins the game at host[:port] (Token: a seat to come back to). */
	void Join(const FString& Address, const FString& PlayerName, uint64 Token = 0);
	/** Runs the server in this program (-GenServer). */
	bool StartServerHere(int32 Port, FString* OutWhy = nullptr);
	/** Leaves the game (the connection is closed; the server stays up for the others). */
	void Leave();

	FGenNetClient* GetClient() const { return Client.Get(); }
	FGenNetServer* GetServer() const { return Server.Get(); }
	bool IsInNetworkGame() const { return Client.IsValid(); }

	/** Where the last network game was and the seat in it, kept on disk: a game that closed in the middle of a
	 *  match comes back to it when started again (-GenRejoin or the menu). */
	void RememberSeat() const;
	bool RecallSeat(FString& OutAddress, FString& OutName, uint64& OutToken) const;
	void ForgetSeat() const;

private:
	TUniquePtr<FGenNetServer> Server;
	TUniquePtr<FGenNetClient> Client;
	FProcHandle ServerProcess;
	FString PlayerName;
};
