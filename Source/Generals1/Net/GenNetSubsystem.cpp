#include "Net/GenNetSubsystem.h"
#include "Generals1.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * Where the seat is kept: a file of its own in the game's saved folder (not the settings file, which every
	 * game running on this computer writes over as a whole). One per install, or - tests running several
	 * games here - one per -GenName.
	 */
	FString SeatFile()
	{
		FString Name;
		return FPaths::ProjectSavedDir() / TEXT("Net") / (FParse::Value(FCommandLine::Get(), TEXT("GenName="), Name) ? TEXT("Seat_") + Name + TEXT(".txt") : FString(TEXT("Seat.txt")));
	}
}

UGenNetSubsystem* UGenNetSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UGenNetSubsystem>() : nullptr;
}

void UGenNetSubsystem::Deinitialize()
{
	Leave();
	if (Server)
	{
		Server->Shutdown();
		Server.Reset();
	}
	Super::Deinitialize();
}

bool UGenNetSubsystem::Host(const FString& InPlayerName, int32 Port, FString* OutWhy)
{
	// The server is this program again, started without a window, sound or a picture.
	const FString Exe = FPlatformProcess::ExecutablePath();
	FString Args;
#if !IS_MONOLITHIC
	// (The editor's build plays the project given to it.)
	Args = FString::Printf(TEXT("\"%s\" /Game/Generals/Maps/Skirmish -game "), *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
#endif
	Args += FString::Printf(TEXT("-GenServer -GenPort=%d -nullrhi -nosound -unattended -nosplash -log=GenServer.log"), Port);
	ServerProcess = FPlatformProcess::CreateProc(*Exe, *Args, true, true, true, nullptr, 0, nullptr, nullptr);
	if (!ServerProcess.IsValid())
	{
		if (OutWhy) { *OutWhy = TEXT("the server can't be started"); }
		UE_LOG(LogGenerals, Error, TEXT("Net: can't start the server: %s %s"), *Exe, *Args);
		return false;
	}
	UE_LOG(LogGenerals, Log, TEXT("Net: the server is starting on port %d"), Port);
	Join(FString::Printf(TEXT("127.0.0.1:%d"), Port), InPlayerName);
	return true;
}

void UGenNetSubsystem::Join(const FString& Address, const FString& InPlayerName, uint64 Token)
{
	Leave();
	PlayerName = InPlayerName;
	Client = MakeUnique<FGenNetClient>();
	Client->Begin(Address, InPlayerName, Token);
}

bool UGenNetSubsystem::StartServerHere(int32 Port, FString* OutWhy)
{
	if (Server)
	{
		return true;
	}
	Server = MakeUnique<FGenNetServer>();
	if (!Server->Start(Port, OutWhy))
	{
		Server.Reset();
		return false;
	}
	return true;
}

void UGenNetSubsystem::Leave()
{
	if (Client)
	{
		Client->Shutdown();
		Client.Reset();
	}
}

void UGenNetSubsystem::RememberSeat() const
{
	if (!Client || Client->GetToken() == 0)
	{
		return;
	}
	// "address", "name", "seat token" - a line each.
	FFileHelper::SaveStringToFile(FString::Printf(TEXT("%s\n%s\n%llu\n"), *Client->GetAddress(), *PlayerName, Client->GetToken()), *SeatFile());
}

bool UGenNetSubsystem::RecallSeat(FString& OutAddress, FString& OutName, uint64& OutToken) const
{
	FString Text;
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToString(Text, *SeatFile()) || Text.ParseIntoArrayLines(Lines) < 3)
	{
		return false;
	}
	OutAddress = Lines[0];
	OutName = Lines[1];
	OutToken = FCString::Strtoui64(*Lines[2], nullptr, 10);
	return OutToken != 0 && !OutAddress.IsEmpty();
}

void UGenNetSubsystem::ForgetSeat() const
{
	IFileManager::Get().Delete(*SeatFile(), false, true, true);
}
