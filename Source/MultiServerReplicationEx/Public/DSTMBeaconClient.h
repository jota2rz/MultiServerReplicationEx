// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MultiServerBeaconClient.h"
#include "DSTMBeaconClient.generated.h"

/**
 * DSTM-aware beacon client for inter-server migration data transfer.
 *
 * Extends AMultiServerBeaconClient with RPCs that carry serialized
 * FRemoteObjectData between game servers. This is the network transport
 * layer that replaces DSTM's default disk I/O with beacon-based delivery.
 *
 * Architecture (see README.md § Migration Flow Reference):
 *   When Server-A calls TransferObjectOwnershipToRemoteServer(), the engine
 *   serializes the actor and invokes RemoteObjectTransferDelegate. Our
 *   DSTMSubsystem catches this, serializes FRemoteObjectData to bytes,
 *   and sends it to Server-B via this beacon's ServerReceiveMigratedObject().
 *
 *   Server-B receives the data, deserializes it, and feeds it into
 *   OnObjectDataReceived() — the engine's DSTM receive pipeline handles
 *   the rest (PostMigrate, connection rebind, etc.).
 *
 * The beacon mesh uses SetUnlimitedBunchSizeAllowed(true) (inherited from
 * AMultiServerBeaconClient), so serialized payloads can exceed the default
 * 64KB reliable RPC limit.
 */
UCLASS(Transient, Config = Engine, NotPlaceable)
class MULTISERVERREPLICATIONEX_API ADSTMBeaconClient : public AMultiServerBeaconClient
{
	GENERATED_BODY()

public:
	ADSTMBeaconClient();

	virtual void OnConnected() override;

	// ──── Migration Data Transfer RPCs ────

	/**
	 * Server RPC: send serialized migration data to the server side of the beacon.
	 * Called from the client side of the beacon connection (HasAuthority() == false).
	 *
	 * @param ObjectIdRaw       - FRemoteObjectId serialized as uint64
	 * @param OwnerServerIdRaw  - FRemoteServerId of the object's owner
	 * @param PhysicsServerIdRaw - FRemoteServerId of the physics simulation owner
	 * @param PhysicsLocalIslandId - Physics island ID on the physics server
	 * @param SenderServerIdRaw - FRemoteServerId of the sending server (us)
	 * @param SerializedData    - FRemoteObjectData serialized to bytes
	 */
	UFUNCTION(Server, Reliable)
	void ServerReceiveMigratedObject(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		const TArray<uint8>& SerializedData);

	/**
	 * Client RPC: send serialized migration data to the client side of the beacon.
	 * Called from the server side of the beacon connection (HasAuthority() == true).
	 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMigratedObject(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		const TArray<uint8>& SerializedData);

	// ──── Pull-Migration Request RPCs ────

	/**
	 * Server RPC: request a remote server to send us a specific object.
	 * Called from the client side of the beacon connection (HasAuthority() == false).
	 * Used for pull-migration: Server-B asks Server-A to migrate an object.
	 *
	 * @param ObjectIdRaw          - FRemoteObjectId of the requested object
	 * @param RequestingServerIdRaw - FRemoteServerId of the requesting server (us)
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestMigrateObject(
		uint64 ObjectIdRaw,
		uint32 RequestingServerIdRaw);

	/**
	 * Client RPC: request migration in the reverse direction.
	 * Called from the server side of the beacon connection (HasAuthority() == true).
	 *
	 * @param ObjectIdRaw          - FRemoteObjectId of the requested object
	 * @param RequestingServerIdRaw - FRemoteServerId of the requesting server (us)
	 */
	UFUNCTION(Client, Reliable)
	void ClientRequestMigrateObject(
		uint64 ObjectIdRaw,
		uint32 RequestingServerIdRaw);

	// ──── Chunked Migration Transfer RPCs ────
	// UE replication limits TArray<uint8> RPC params to 65535 elements.
	// Large actors (e.g. pawns with components) can exceed this.
	// These RPCs split the payload into chunks and reassemble on receive.

	/** Server RPC: send one chunk of a large migration payload. */
	UFUNCTION(Server, Reliable)
	void ServerReceiveMigratedObjectChunk(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		int32 ChunkIndex,
		int32 TotalChunks,
		int32 TotalSize,
		const TArray<uint8>& ChunkData);

	/** Client RPC: send one chunk of a large migration payload. */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMigratedObjectChunk(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		int32 ChunkIndex,
		int32 TotalChunks,
		int32 TotalSize,
		const TArray<uint8>& ChunkData);

	// ──── Delegates ────

	DECLARE_MULTICAST_DELEGATE_SixParams(FOnMigrationDataReceived,
		uint64 /* ObjectIdRaw */,
		uint32 /* OwnerServerIdRaw */,
		uint32 /* PhysicsServerIdRaw */,
		uint32 /* PhysicsLocalIslandId */,
		uint32 /* SenderServerIdRaw */,
		const TArray<uint8>& /* SerializedData */);

	/** Fired when migration data arrives on this beacon (from either direction). */
	FOnMigrationDataReceived OnMigrationDataReceived;

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMigrationRequested,
		uint64 /* ObjectIdRaw */,
		uint32 /* RequestingServerIdRaw */);

	/** Fired when a peer requests us to send them an object. */
	FOnMigrationRequested OnMigrationRequested;

	// ──── Chunk Reassembly ────

	struct FChunkAssembly
	{
		uint32 OwnerServerIdRaw = 0;
		uint32 PhysicsServerIdRaw = 0;
		uint32 PhysicsLocalIslandId = 0;
		uint32 SenderServerIdRaw = 0;
		int32 TotalChunks = 0;
		int32 TotalSize = 0;
		int32 ChunksReceived = 0;
		TArray<uint8> ReassembledData;
	};

	/** Pending chunk reassembly buffers, keyed by ObjectIdRaw. */
	TMap<uint64, FChunkAssembly> PendingChunks;

private:
	/** Process a received chunk and fire delegate when all chunks arrive. */
	void HandleReceivedChunk(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		int32 ChunkIndex,
		int32 TotalChunks,
		int32 TotalSize,
		const TArray<uint8>& ChunkData);
};
