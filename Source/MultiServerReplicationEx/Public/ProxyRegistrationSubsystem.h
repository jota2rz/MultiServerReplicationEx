// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ProxyRegistrationSubsystem.generated.h"

/**
 * Auto-detects proxy registration command-line flags and activates
 * the appropriate HTTP registration path on world begin play.
 *
 * Proxy server (-ProxyRegistrationPort=): starts the HTTP listener.
 * Game server  (-JoinProxy=):             sends HTTP POST to register.
 *
 * Only created on dedicated servers.
 */
UCLASS()
class MULTISERVERREPLICATIONEX_API UProxyRegistrationSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
};
