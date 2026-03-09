// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#if UE_WITH_REMOTE_OBJECT_HANDLE
#include "UObject/RemoteObjectTransfer.h"
#include "UObject/RemoteObjectTypes.h"
#endif

#include "DSTMSubsystem.generated.h"

class UMultiServerNode;
class AMultiServerBeaconClient;
class ADSTMBeaconClient;

/**
 * Manages the DSTM beacon transport mesh for seamless cross-server migration.
 *
 * This subsystem is the runtime counterpart of the DSTMTransportModule.
 * The module binds static delegate callbacks at startup; those callbacks
 * forward to this subsystem for actual routing through the beacon mesh.
 *
 * Architecture (see README.md):
 *   ┌──────────────┐       ┌──────────────────────┐       ┌──────────────┐
 *   │   Server-A   │       │   DSTM Beacon Mesh   │       │   Server-B   │
 *   │              │       │                      │       │              │
 *   │ TransferOwn  │──────►│  Serialize + Send    │──────►│  Receive +   │
 *   │  ership()    │       │  via BeaconClient    │       │  Deserialize │
 *   │              │       │  RPC                 │       │              │
 *   │ PostMigrate  │       │                      │       │ OnObjectData │
 *   │  (Send)      │       │                      │       │  Received()  │
 *   │              │       │                      │       │              │
 *   │ [Migrated    │       │                      │       │ PostMigrate  │
 *   │  close]      │       │                      │       │  (Receive)   │
 *   └──────────────┘       └──────────────────────┘       └──────────────┘
 *
 * The mesh is separate from any game-specific MultiServer mesh to maintain
 * plugin isolation. It reads the same command-line peer config with an
 * offset port (+1000).
 *
 * Lifecycle:
 *   1. DSTMTransportModule::StartupModule() binds transport delegates
 *   2. This subsystem initializes and creates the beacon mesh
 *   3. On peer connection, migration data can flow
 *   4. On shutdown, mesh is torn down gracefully
 */
UCLASS()
class DSTMTRANSPORT_API UDSTMSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ──── Mesh Management ────

	/**
	 * Initialize the DSTM beacon mesh from command-line arguments.
	 * Reads -DedicatedServerId=, -MultiServerListenPort=, -MultiServerPeers=
	 * and uses an offset port for the DSTM mesh.
	 *
	 * @return true if DSTM mesh was configured and created
	 */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	bool InitializeFromCommandLine();

	/**
	 * Initialize the DSTM beacon mesh with explicit parameters.
	 *
	 * NumServers is derived automatically as PeerAddresses.Num() + 1 (peers + self).
	 *
	 * @param LocalPeerId    - This server's unique ID string
	 * @param ListenIp       - IP to listen on for beacon connections
	 * @param ListenPort     - Port for DSTM beacon listener
	 * @param PeerAddresses  - Array of "IP:Port" for DSTM beacons on other servers
	 */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	void InitializeDSTMMesh(
		const FString& LocalPeerId,
		const FString& ListenIp,
		int32 ListenPort,
		const TArray<FString>& PeerAddresses);

	/** Is the DSTM mesh active and ready for migration? */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	bool IsMeshActive() const { return DSTMNode != nullptr; }

	/** Are all expected DSTM peer connections established? */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	bool AreAllPeersConnected() const;

	/** Get the number of currently valid (connected) peers. */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	int32 GetConnectedPeerCount() const;

	/** Get the peer IDs of all currently valid (connected) peers. */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	TArray<FString> GetConnectedPeerIds() const;

	/** Shut down the DSTM mesh. */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	void ShutdownMesh();

	/**
	 * Apply a GUID seed to prevent FNetworkGUID collisions between backend servers.
	 * Each server in a multi-server cluster must use a distinct seed value so that
	 * independently spawned actors (PlayerControllers, Pawns, etc.) get non-overlapping
	 * GUIDs in the proxy's shared backend GUID cache.
	 *
	 * This is a separate concern from DSTM's FRemoteObjectId identity system — GuidSeed
	 * prevents proxy-level GUID collisions during normal replication, while DSTM handles
	 * cross-server object identity during migration.
	 *
	 * Call this before any clients connect (typically from GameMode::StartPlay()).
	 *
	 * @param GuidSeed - Non-zero seed value (e.g. 100000 for server-1, 200000 for server-2)
	 */
	UFUNCTION(BlueprintCallable, Category = "DSTM")
	void ApplyGuidSeed(uint64 GuidSeed);

	// ──── Migration API ────

#if UE_WITH_REMOTE_OBJECT_HANDLE
	/**
	 * Transfer an actor to a remote server using the DSTM pipeline.
	 * Calls UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer().
	 *
	 * @param Actor          - The actor to transfer (typically PlayerController or Pawn)
	 * @param DestServerId   - Destination server's FRemoteServerId
	 */
	void TransferActorToServer(AActor* Actor, FRemoteServerId DestServerId);

	/**
	 * Get the FRemoteServerId for a peer by its DedicatedServerId string.
	 * Uses the same hash as InitializeServerIdentity() for consistency.
	 */
	static FRemoteServerId GetRemoteServerIdFromString(const FString& DedicatedServerId);

	/**
	 * Get the FRemoteServerId for the first connected peer.
	 * Convenience for 2-server setups where there's only one peer.
	 *
	 * @param OutServerId - Receives the peer's FRemoteServerId
	 * @return true if a peer is connected
	 */
	bool GetFirstPeerServerId(FRemoteServerId& OutServerId) const;

	// ──── Transport Delegate Handlers ────
	// Called by the module's static callbacks.

	/** Handle outgoing migration: serialize FRemoteObjectData and send via beacon. */
	void HandleOutgoingMigration(const UE::RemoteObject::Transfer::FMigrateSendParams& Params);

	/** Handle incoming pull-request: forward to the appropriate peer. */
	void HandleObjectRequest(
		FRemoteWorkPriority Priority,
		FRemoteObjectId ObjectId,
		FRemoteServerId LastKnownServerId,
		FRemoteServerId DestServerId);
#endif

	// ──── Port Offset ────

	/** Port offset from the main MultiServer mesh port. Default: 1000. */
	static constexpr int32 DSTMPortOffset = 1000;

private:
	void HandlePeerConnected(
		const FString& LocalPeerId,
		const FString& RemotePeerId,
		AMultiServerBeaconClient* Beacon);

#if UE_WITH_REMOTE_OBJECT_HANDLE
	/** Handle incoming migration data from a peer beacon. */
	void HandleIncomingMigrationData(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		const TArray<uint8>& SerializedData);

	/** Handle incoming pull-migration request from a peer beacon. */
	void HandleIncomingMigrationRequest(
		uint64 ObjectIdRaw,
		uint32 RequestingServerIdRaw);
#endif

	/**
	 * Rewrite peer addresses to use the DSTM port offset.
	 * "192.168.1.10:15000" → "192.168.1.10:16000" (with offset 1000)
	 */
	static TArray<FString> OffsetPeerPorts(const TArray<FString>& PeerAddresses, int32 Offset);

	/** Find the beacon client connected to a specific server. */
	ADSTMBeaconClient* FindBeaconForServer(uint32 ServerIdHash) const;

	UPROPERTY()
	TObjectPtr<UMultiServerNode> DSTMNode;

	/** Map from peer DedicatedServerId string → beacon client. */
	UPROPERTY()
	TMap<FString, TObjectPtr<ADSTMBeaconClient>> PeerBeacons;

	/** Map from hashed server ID → peer DedicatedServerId string (reverse lookup). */
	TMap<uint32, FString> ServerIdHashToPeerId;
};
