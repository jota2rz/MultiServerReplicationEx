// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProxyRegistrationSubsystem.h"
#include "MultiServerProxy.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"

DEFINE_LOG_CATEGORY_STATIC(LogProxyRegistration, Log, All);

bool UProxyRegistrationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Only on dedicated servers — clients have no use for proxy registration.
	if (!IsRunningDedicatedServer())
	{
		return false;
	}

	// Only if one of the proxy flags is present on the command line.
	const TCHAR* CmdLine = FCommandLine::Get();
	return FString(CmdLine).Contains(TEXT("-ProxyRegistrationPort="))
		|| FString(CmdLine).Contains(TEXT("-JoinProxy="));
}

void UProxyRegistrationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	const TCHAR* CmdLine = FCommandLine::Get();

	// ── Proxy side: start the HTTP registration listener ──
	int32 RegistrationPort = 0;
	if (FParse::Value(CmdLine, TEXT("-ProxyRegistrationPort="), RegistrationPort))
	{
		UProxyNetDriver* ProxyDriver = Cast<UProxyNetDriver>(InWorld.GetNetDriver());
		if (ProxyDriver)
		{
			ProxyDriver->StartRegistrationHTTP(RegistrationPort);
		}
		else
		{
			UE_LOG(LogProxyRegistration, Error, TEXT("Net driver is not UProxyNetDriver — cannot start HTTP registration"));
		}
		return;
	}

	// ── Game server side: register with the proxy via HTTP POST ──
	FString JoinProxyAddr;
	if (FParse::Value(CmdLine, TEXT("-JoinProxy="), JoinProxyAddr, false))
	{
		UProxyNetDriver::JoinProxyHTTP(&InWorld);
	}
}
