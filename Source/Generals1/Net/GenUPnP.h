#pragma once

#include "CoreMinimal.h"

/**
 * Opening the server's port on the home router (UPnP Internet Gateway Device): the router is found on the
 * local network (SSDP), asked to forward the port to this computer (AddPortMapping) and asked for the public
 * address the internet sees (GetExternalIPAddress) - what the host's friends type in to join. Everything here
 * blocks for a few seconds at most: it is called from the server's own threads.
 */
namespace GenUPnP
{
	struct FResult
	{
		bool bFoundRouter = false;
		bool bPortOpen = false;
		FString Router;         // what the router calls itself
		FString ExternalIP;     // the address the internet sees (empty: not known)
		FString LocalIP;        // this computer's on the home network
		FString Error;          // why not, when it didn't work
	};

	/** Forwards TCP Port on the router to this computer (until ClosePort, or for a week). */
	GENERALS1_API FResult OpenPort(int32 Port, const FString& Description);
	/** Takes the forwarding back (the router OpenPort found). */
	GENERALS1_API void ClosePort(int32 Port);
}
