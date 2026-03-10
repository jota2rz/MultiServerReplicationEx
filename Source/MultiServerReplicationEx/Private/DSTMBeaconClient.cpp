// Copyright Epic Games, Inc. All Rights Reserved.

#include "DSTMBeaconClient.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(DSTMBeaconClient)

DEFINE_LOG_CATEGORY_STATIC(LogDSTMBeacon, Log, All);

// ─── Constructor ──────────────────────────────────────────────────

ADSTMBeaconClient::ADSTMBeaconClient()
{
}

// ─── Connection ───────────────────────────────────────────────────

void ADSTMBeaconClient::OnConnected()
{
	Super::OnConnected();

	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTMBeacon: Connected to peer %s (local=%s) — DSTM transport ready"),
		*GetRemotePeerId(), *GetLocalPeerId());
}

// ─── Migration Data Transfer RPCs ─────────────────────────────────

void ADSTMBeaconClient::ServerReceiveMigratedObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	const TArray<uint8>& SerializedData)
{
	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Recv [Server RPC]: ObjectId=%llu, Owner=%u, Physics=%u, Sender=%u, DataSize=%d bytes"),
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw, SenderServerIdRaw, SerializedData.Num());

	OnMigrationDataReceived.Broadcast(
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw,
		PhysicsLocalIslandId, SenderServerIdRaw, SerializedData);
}

void ADSTMBeaconClient::ClientReceiveMigratedObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	const TArray<uint8>& SerializedData)
{
	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Recv [Client RPC]: ObjectId=%llu, Owner=%u, Physics=%u, Sender=%u, DataSize=%d bytes"),
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw, SenderServerIdRaw, SerializedData.Num());

	OnMigrationDataReceived.Broadcast(
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw,
		PhysicsLocalIslandId, SenderServerIdRaw, SerializedData);
}

// ─── Pull-Migration Request RPCs ──────────────────────────────────

bool ADSTMBeaconClient::ServerRequestMigrateObject_Validate(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	// Basic validation: object ID should be non-zero
	return ObjectIdRaw != 0;
}

void ADSTMBeaconClient::ServerRequestMigrateObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Request [Server RPC]: ObjectId=%llu requested by server %u"),
		ObjectIdRaw, RequestingServerIdRaw);

	OnMigrationRequested.Broadcast(ObjectIdRaw, RequestingServerIdRaw);
}

void ADSTMBeaconClient::ClientRequestMigrateObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Request [Client RPC]: ObjectId=%llu requested by server %u"),
		ObjectIdRaw, RequestingServerIdRaw);

	OnMigrationRequested.Broadcast(ObjectIdRaw, RequestingServerIdRaw);
}

// ─── Chunked Migration Transfer RPCs ──────────────────────────────

void ADSTMBeaconClient::ServerReceiveMigratedObjectChunk_Implementation(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	int32 ChunkIndex,
	int32 TotalChunks,
	int32 TotalSize,
	const TArray<uint8>& ChunkData)
{
	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Chunk Recv [Server RPC]: ObjectId=%llu, Chunk %d/%d (%d bytes)"),
		ObjectIdRaw, ChunkIndex + 1, TotalChunks, ChunkData.Num());

	HandleReceivedChunk(ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw,
		PhysicsLocalIslandId, SenderServerIdRaw, ChunkIndex, TotalChunks,
		TotalSize, ChunkData);
}

void ADSTMBeaconClient::ClientReceiveMigratedObjectChunk_Implementation(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	int32 ChunkIndex,
	int32 TotalChunks,
	int32 TotalSize,
	const TArray<uint8>& ChunkData)
{
	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Chunk Recv [Client RPC]: ObjectId=%llu, Chunk %d/%d (%d bytes)"),
		ObjectIdRaw, ChunkIndex + 1, TotalChunks, ChunkData.Num());

	HandleReceivedChunk(ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw,
		PhysicsLocalIslandId, SenderServerIdRaw, ChunkIndex, TotalChunks,
		TotalSize, ChunkData);
}

void ADSTMBeaconClient::HandleReceivedChunk(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	int32 ChunkIndex,
	int32 TotalChunks,
	int32 TotalSize,
	const TArray<uint8>& ChunkData)
{
	FChunkAssembly& Assembly = PendingChunks.FindOrAdd(ObjectIdRaw);

	// Initialize on first chunk
	if (Assembly.TotalChunks == 0)
	{
		Assembly.OwnerServerIdRaw = OwnerServerIdRaw;
		Assembly.PhysicsServerIdRaw = PhysicsServerIdRaw;
		Assembly.PhysicsLocalIslandId = PhysicsLocalIslandId;
		Assembly.SenderServerIdRaw = SenderServerIdRaw;
		Assembly.TotalChunks = TotalChunks;
		Assembly.TotalSize = TotalSize;
		Assembly.ChunksReceived = 0;
		Assembly.ReassembledData.SetNumZeroed(TotalSize);
	}

	// Copy chunk data into the correct position
	const int32 Offset = ChunkIndex * 60000; // Must match MaxChunkSize in DSTMSubsystem
	const int32 BytesToCopy = FMath::Min(ChunkData.Num(), TotalSize - Offset);
	FMemory::Memcpy(Assembly.ReassembledData.GetData() + Offset, ChunkData.GetData(), BytesToCopy);
	Assembly.ChunksReceived++;

	UE_LOG(LogDSTMBeacon, Log,
		TEXT("DSTM Chunk: ObjectId=%llu — received %d/%d chunks"),
		ObjectIdRaw, Assembly.ChunksReceived, Assembly.TotalChunks);

	// All chunks received — fire delegate with the full reassembled data
	if (Assembly.ChunksReceived >= Assembly.TotalChunks)
	{
		UE_LOG(LogDSTMBeacon, Log,
			TEXT("DSTM Chunk: ObjectId=%llu — all %d chunks received, reassembled %d bytes — forwarding"),
			ObjectIdRaw, Assembly.TotalChunks, Assembly.ReassembledData.Num());

		OnMigrationDataReceived.Broadcast(
			ObjectIdRaw, Assembly.OwnerServerIdRaw, Assembly.PhysicsServerIdRaw,
			Assembly.PhysicsLocalIslandId, Assembly.SenderServerIdRaw,
			Assembly.ReassembledData);

		PendingChunks.Remove(ObjectIdRaw);
	}
}
