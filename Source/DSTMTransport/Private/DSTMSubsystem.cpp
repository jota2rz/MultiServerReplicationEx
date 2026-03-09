// Copyright Epic Games, Inc. All Rights Reserved.

#include "DSTMSubsystem.h"
#include "DSTMBeaconClient.h"
#include "MultiServerNode.h"
#include "MultiServerBeaconClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "EngineUtils.h" // TActorIterator
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h" // FNetGUIDCache

#if UE_WITH_REMOTE_OBJECT_HANDLE
#include "UObject/RemoteObjectTransfer.h"
#include "UObject/RemoteObjectTypes.h"
#include "UObject/RemoteObjectPathName.h" // FRemoteObjectTables, FPackedRemoteObjectPathName operator<<
#include "UObject/UObjectMigrationContext.h" // FUObjectMigrationContext — direct member access replaces GetMigrationRoutingInfo()
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(DSTMSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogDSTMSub, Log, All);

#if UE_WITH_REMOTE_OBJECT_HANDLE
/**
 * Serialize/deserialize FRemoteObjectData to/from an FArchive.
 * FRemoteObjectData has no built-in operator<< — we serialize individual members.
 * Works bidirectionally: FMemoryWriter (save) and FMemoryReader (load).
 *
 * - Tables: has exported operator<< in RemoteObjectPathName.h
 * - PathNames: TArray<FPackedRemoteObjectPathName>, each has exported operator<<
 * - Bytes: FRemoteObjectBytes only has a file-scoped operator<< in engine internals,
 *          so we serialize each chunk's TArray<uint8> manually
 */
static void ArchiveRemoteObjectData(FArchive& Ar, FRemoteObjectData& Data)
{
	Ar << Data.Tables;
	Ar << Data.PathNames;

	int32 NumChunks = Data.Bytes.Num();
	Ar << NumChunks;
	if (Ar.IsLoading())
	{
		Data.Bytes.SetNum(NumChunks);
	}
	for (FRemoteObjectBytes& Chunk : Data.Bytes)
	{
		Ar << Chunk.Bytes;
	}
}
#endif // UE_WITH_REMOTE_OBJECT_HANDLE

// ─── Lifecycle ────────────────────────────────────────────────────

bool UDSTMSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Always create — inert until InitializeDSTMMesh() or InitializeFromCommandLine()
	return true;
}

void UDSTMSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTMSubsystem initialized (inert until DSTM mesh setup)"));

	// Do NOT auto-initialize here — GetWorld() returns nullptr during
	// GameInstance subsystem initialization because no World has been
	// created yet. The game mode calls InitializeFromCommandLine()
	// from StartPlay() when the World is ready.
}

void UDSTMSubsystem::Deinitialize()
{
	ShutdownMesh();
	Super::Deinitialize();
}

// ─── Mesh Management ─────────────────────────────────────────────

bool UDSTMSubsystem::InitializeFromCommandLine()
{
#if !UE_WITH_REMOTE_OBJECT_HANDLE
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM mesh not available: UE_WITH_REMOTE_OBJECT_HANDLE is disabled"));
	return false;
#else
	// Check if multi-server mode is requested via command line.
	// Uses -DedicatedServerId= as both the DSTM server identity and the
	// beacon mesh LocalPeerId — no separate -MultiServerLocalId= needed.
	FString LocalPeerId;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-DedicatedServerId="), LocalPeerId, false))
	{
		// Not in multi-server mode — no mesh needed
		return false;
	}

	FString ListenIp = TEXT("0.0.0.0");
	FParse::Value(FCommandLine::Get(), TEXT("-MultiServerListenIp="), ListenIp, false);

	int32 BaseListenPort = 15000;
	FParse::Value(FCommandLine::Get(), TEXT("-MultiServerListenPort="), BaseListenPort);

	// Apply DSTM port offset
	const int32 DSTMListenPort = BaseListenPort + DSTMPortOffset;

	FString PeersArg;
	TArray<FString> BasePeerAddresses;
	if (FParse::Value(FCommandLine::Get(), TEXT("-MultiServerPeers="), PeersArg, false))
	{
		PeersArg.ParseIntoArray(BasePeerAddresses, TEXT(","), true);
	}

	// Rewrite peer addresses with DSTM port offset
	TArray<FString> DSTMPeerAddresses = OffsetPeerPorts(BasePeerAddresses, DSTMPortOffset);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM mesh auto-init: LocalId=%s, DSTMPort=%d (base %d + offset %d), Peers=%d"),
		*LocalPeerId, DSTMListenPort, BaseListenPort, DSTMPortOffset, DSTMPeerAddresses.Num());

	InitializeDSTMMesh(LocalPeerId, ListenIp, DSTMListenPort, DSTMPeerAddresses);

	// Apply GUID seed if specified (prevents FNetworkGUID collisions between servers)
	uint64 GuidSeed = 0;
	FParse::Value(FCommandLine::Get(), TEXT("-DSTMGuidSeed="), GuidSeed);
	if (GuidSeed > 0)
	{
		ApplyGuidSeed(GuidSeed);
	}

	return DSTMNode != nullptr;
#endif
}

void UDSTMSubsystem::InitializeDSTMMesh(
	const FString& LocalPeerId,
	const FString& ListenIp,
	int32 ListenPort,
	const TArray<FString>& PeerAddresses)
{
	if (DSTMNode)
	{
		UE_LOG(LogDSTMSub, Warning,
			TEXT("DSTM mesh already active. Ignoring re-initialization."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("Cannot initialize DSTM mesh — no World"));
		return;
	}

	// Derive expected server count from peer list (peers + self).
	// This feeds UMultiServerNode::NumExpectedServers, used only by
	// AreAllServersConnected() as a startup readiness check.
	const int32 NumServers = PeerAddresses.Num() + 1;

	FMultiServerNodeCreateParams Params;
	Params.World = World;
	Params.LocalPeerId = LocalPeerId;
	Params.ListenIp = ListenIp;
	Params.ListenPort = static_cast<uint16>(ListenPort);
	Params.NumServers = static_cast<uint32>(NumServers);
	Params.PeerAddresses = PeerAddresses;
	Params.UserBeaconClass = ADSTMBeaconClient::StaticClass();
	Params.OnMultiServerConnected.BindUObject(
		this, &UDSTMSubsystem::HandlePeerConnected);

	DSTMNode = UMultiServerNode::Create(Params);

	if (DSTMNode)
	{
		UE_LOG(LogDSTMSub, Log,
			TEXT("DSTM mesh created: LocalPeerId=%s, ListenPort=%d, NumServers=%d, Peers=%d"),
			*LocalPeerId, ListenPort, NumServers, PeerAddresses.Num());

		for (const FString& Addr : PeerAddresses)
		{
			UE_LOG(LogDSTMSub, Log, TEXT("  DSTM Peer: %s"), *Addr);
		}
	}
	else
	{
		UE_LOG(LogDSTMSub, Error, TEXT("Failed to create DSTM mesh"));
	}
}

bool UDSTMSubsystem::AreAllPeersConnected() const
{
	return DSTMNode && DSTMNode->AreAllServersConnected();
}

int32 UDSTMSubsystem::GetConnectedPeerCount() const
{
	int32 Count = 0;
	for (const auto& Pair : PeerBeacons)
	{
		if (Pair.Value && IsValid(Pair.Value))
		{
			Count++;
		}
	}
	return Count;
}

TArray<FString> UDSTMSubsystem::GetConnectedPeerIds() const
{
	TArray<FString> Result;
	for (const auto& Pair : PeerBeacons)
	{
		if (Pair.Value && IsValid(Pair.Value))
		{
			Result.Add(Pair.Key);
		}
	}
	return Result;
}

void UDSTMSubsystem::ShutdownMesh()
{
	if (DSTMNode)
	{
		UE_LOG(LogDSTMSub, Log, TEXT("Shutting down DSTM mesh"));
		// UMultiServerNode handles cleanup in BeginDestroy
		DSTMNode = nullptr;
		PeerBeacons.Empty();
		ServerIdHashToPeerId.Empty();
	}
}

// ─── Migration API ───────────────────────────────────────────────

void UDSTMSubsystem::ApplyGuidSeed(uint64 GuidSeed)
{
	if (GuidSeed == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogDSTMSub, Warning,
			TEXT("ApplyGuidSeed: No World — cannot apply GUID seed"));
		return;
	}

	UNetDriver* NetDriver = World->GetNetDriver();
	if (!NetDriver)
	{
		UE_LOG(LogDSTMSub, Warning,
			TEXT("ApplyGuidSeed: No NetDriver — cannot apply GUID seed"));
		return;
	}

	// Use GetNetGuidCache() accessor instead of direct member access.
	// UNetDriver::GuidCache is slated for deprecation (UE_DEPRECATED 5.6,
	// currently commented out). The non-const GetNetGuidCache() returns a
	// TSharedPtr<FNetGUIDCache>& that we can assign through.
	NetDriver->GetNetGuidCache() = MakeShared<FNetGUIDCache>(NetDriver, GuidSeed);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM: Applied GUID seed %llu to NetDriver GuidCache"), GuidSeed);
}

#if UE_WITH_REMOTE_OBJECT_HANDLE

void UDSTMSubsystem::TransferActorToServer(AActor* Actor, FRemoteServerId DestServerId)
{
	if (!Actor)
	{
		UE_LOG(LogDSTMSub, Error, TEXT("TransferActorToServer: Actor is null"));
		return;
	}

	if (!DSTMNode)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("TransferActorToServer: DSTM mesh not active — cannot transfer %s"),
			*Actor->GetName());
		return;
	}

	UE_LOG(LogDSTMSub, Log,
		TEXT("TransferActorToServer: Initiating DSTM transfer of %s to server %u"),
		*Actor->GetName(), DestServerId.GetIdNumber());

	// This one call does everything:
	// 1. Serializes the actor + all subobjects
	// 2. Calls AActor::PostMigrate(Send) — world removal, channel close with Migrated
	// 3. Calls APlayerController::PostMigrate(Send) — NoPawnPC swap, connection save
	// 4. Invokes RemoteObjectTransferDelegate → our HandleOutgoingMigration() sends via beacon
	UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer(Actor, DestServerId);
}

FRemoteServerId UDSTMSubsystem::GetRemoteServerIdFromString(const FString& DedicatedServerId)
{
	return FRemoteServerId::FromIdNumber(GetTypeHash(DedicatedServerId));
}

bool UDSTMSubsystem::GetFirstPeerServerId(FRemoteServerId& OutServerId) const
{
	for (const auto& Pair : ServerIdHashToPeerId)
	{
		OutServerId = FRemoteServerId::FromIdNumber(Pair.Key);
		return true;
	}
	return false;
}

// ─── Transport Delegate Handlers ──────────────────────────────────

void UDSTMSubsystem::HandleOutgoingMigration(
	const UE::RemoteObject::Transfer::FMigrateSendParams& Params)
{
	// Extract routing info directly from the migration context (Internal header)
	const FUObjectMigrationContext& Ctx = Params.MigrationContext;

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Send: ObjectId=%llu → DestServer=%u (Owner=%u, Physics=%u)"),
		Ctx.ObjectId.GetIdNumber(),
		Ctx.RemoteServerId.GetIdNumber(),
		Ctx.OwnerServerId.GetIdNumber(),
		Ctx.PhysicsServerId.GetIdNumber());

	// Serialize FRemoteObjectData to a byte array for network transfer.
	// Make a copy since archive serialization requires a non-const reference.
	TArray<uint8> SerializedData;
	FMemoryWriter Writer(SerializedData);
	FRemoteObjectData ObjectDataCopy = Params.ObjectData;
	ArchiveRemoteObjectData(Writer, ObjectDataCopy);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Send: Serialized %d bytes of object data"),
		SerializedData.Num());

	// Find the beacon connected to the destination server
	ADSTMBeaconClient* Beacon = FindBeaconForServer(
		Ctx.RemoteServerId.GetIdNumber());

	if (!Beacon)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Send: No beacon connection to destination server %u! Migration data lost."),
			Ctx.RemoteServerId.GetIdNumber());
		return;
	}

	// Send via the appropriate RPC direction based on beacon authority
	const uint32 LocalServerId = FRemoteServerId::GetLocalServerId().GetIdNumber();

	if (Beacon->IsAuthorityBeacon())
	{
		// We are the server side of this beacon connection → use Client RPC
		Beacon->ClientReceiveMigratedObject(
			Ctx.ObjectId.GetIdNumber(),
			Ctx.OwnerServerId.GetIdNumber(),
			Ctx.PhysicsServerId.GetIdNumber(),
			Ctx.PhysicsLocalIslandId,
			LocalServerId,
			SerializedData);
	}
	else
	{
		// We are the client side → use Server RPC
		Beacon->ServerReceiveMigratedObject(
			Ctx.ObjectId.GetIdNumber(),
			Ctx.OwnerServerId.GetIdNumber(),
			Ctx.PhysicsServerId.GetIdNumber(),
			Ctx.PhysicsLocalIslandId,
			LocalServerId,
			SerializedData);
	}

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Send: Migration data dispatched via %s RPC (%d bytes)"),
		Beacon->IsAuthorityBeacon() ? TEXT("Client") : TEXT("Server"),
		SerializedData.Num());
}

void UDSTMSubsystem::HandleObjectRequest(
	FRemoteWorkPriority Priority,
	FRemoteObjectId ObjectId,
	FRemoteServerId LastKnownServerId,
	FRemoteServerId DestServerId)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Request: Object %llu — requesting from server %u for destination %u"),
		ObjectId.GetIdNumber(),
		LastKnownServerId.GetIdNumber(),
		DestServerId.GetIdNumber());

	ADSTMBeaconClient* Beacon = FindBeaconForServer(LastKnownServerId.GetIdNumber());
	if (Beacon)
	{
		// Send via the appropriate RPC direction based on beacon authority
		if (Beacon->IsAuthorityBeacon())
		{
			// We are the server side of this beacon connection → use Client RPC
			Beacon->ClientRequestMigrateObject(
				ObjectId.GetIdNumber(),
				DestServerId.GetIdNumber());
		}
		else
		{
			// We are the client side → use Server RPC
			Beacon->ServerRequestMigrateObject(
				ObjectId.GetIdNumber(),
				DestServerId.GetIdNumber());
		}

		UE_LOG(LogDSTMSub, Log,
			TEXT("DSTM Request: Dispatched via %s RPC"),
			Beacon->IsAuthorityBeacon() ? TEXT("Client") : TEXT("Server"));
	}
	else
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Request: No beacon connection to server %u"),
			LastKnownServerId.GetIdNumber());
	}
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE

// ─── Peer Connection Handling ─────────────────────────────────────

void UDSTMSubsystem::HandlePeerConnected(
	const FString& LocalPeerId,
	const FString& RemotePeerId,
	AMultiServerBeaconClient* Beacon)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM peer connected: %s → %s"), *LocalPeerId, *RemotePeerId);

	ADSTMBeaconClient* DSTMBeacon = Cast<ADSTMBeaconClient>(Beacon);
	if (!DSTMBeacon)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM peer connected but beacon is not ADSTMBeaconClient!"));
		return;
	}

	// Handle server rejoin: if this peer was previously connected (e.g. after
	// a crash/restart), unbind delegates from the old beacon to prevent
	// duplicate migration callbacks. The old beacon actor may already be
	// destroyed, but RemoveAll is safe on dead objects.
	if (TObjectPtr<ADSTMBeaconClient>* OldBeaconPtr = PeerBeacons.Find(RemotePeerId))
	{
		ADSTMBeaconClient* OldBeacon = OldBeaconPtr->Get();
		if (OldBeacon && OldBeacon != DSTMBeacon)
		{
			UE_LOG(LogDSTMSub, Log,
				TEXT("DSTM peer '%s' reconnected — unbinding delegates from previous beacon"),
				*RemotePeerId);
#if UE_WITH_REMOTE_OBJECT_HANDLE
			OldBeacon->OnMigrationDataReceived.RemoveAll(this);
			OldBeacon->OnMigrationRequested.RemoveAll(this);
#endif
		}
	}

	PeerBeacons.Add(RemotePeerId, DSTMBeacon);

	// Store reverse lookup: hash → peer ID string.
	// Detect hash collisions — two different DedicatedServerId strings that
	// produce the same GetTypeHash() would silently misroute migration data.
	const uint32 PeerHash = GetTypeHash(RemotePeerId);
	if (const FString* Existing = ServerIdHashToPeerId.Find(PeerHash))
	{
		if (*Existing != RemotePeerId)
		{
			UE_LOG(LogDSTMSub, Error,
				TEXT("DSTM HASH COLLISION: DedicatedServerId '%s' and '%s' both hash to %u! "
					"Migration routing will be BROKEN. Rename one of the server IDs."),
				**Existing, *RemotePeerId, PeerHash);
		}
	}
	ServerIdHashToPeerId.Add(PeerHash, RemotePeerId);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM peer registered: '%s' → hash %u"), *RemotePeerId, PeerHash);

#if UE_WITH_REMOTE_OBJECT_HANDLE
	// Wire up delegates for incoming migration data from this peer
	DSTMBeacon->OnMigrationDataReceived.AddUObject(
		this, &UDSTMSubsystem::HandleIncomingMigrationData);
	DSTMBeacon->OnMigrationRequested.AddUObject(
		this, &UDSTMSubsystem::HandleIncomingMigrationRequest);
#endif
}

#if UE_WITH_REMOTE_OBJECT_HANDLE

void UDSTMSubsystem::HandleIncomingMigrationData(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	const TArray<uint8>& SerializedData)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Recv: ObjectId=%llu, Owner=%u, Sender=%u, DataSize=%d bytes — feeding to engine"),
		ObjectIdRaw, OwnerServerIdRaw, SenderServerIdRaw, SerializedData.Num());

	// Deserialize FRemoteObjectData from the byte array
	FRemoteObjectData ObjectData;
	FMemoryReader Reader(SerializedData);
	ArchiveRemoteObjectData(Reader, ObjectData);

	if (Reader.IsError())
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Recv: Failed to deserialize FRemoteObjectData — "
				"ObjectId=%llu, Owner=%u, Sender=%u, DataSize=%d bytes. "
				"Possible version mismatch or corrupted payload."),
			ObjectIdRaw, OwnerServerIdRaw, SenderServerIdRaw, SerializedData.Num());
		return;
	}

	// Feed the deserialized data into the engine's DSTM receive pipeline.
	// This triggers:
	//   1. FRemoteObjectTransferQueue::FulfillReceiveRequest()
	//   2. Deserializes the actor (same C++ object, same FRemoteObjectId)
	//   3. AActor::PostMigrate(Receive) → adds to world, starts replicating
	//   4. APlayerController::PostMigrate(Receive) → finds connection, binds PC
	UE::RemoteObject::Transfer::OnObjectDataReceived(
		FRemoteServerId::FromIdNumber(OwnerServerIdRaw),
		FRemoteServerId::FromIdNumber(PhysicsServerIdRaw),
		PhysicsLocalIslandId,
		FRemoteObjectId::CreateFromInt(ObjectIdRaw),
		FRemoteServerId::FromIdNumber(SenderServerIdRaw),
		ObjectData);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Recv: ObjectId=%llu delivered to engine receive pipeline"), ObjectIdRaw);
}

void UDSTMSubsystem::HandleIncomingMigrationRequest(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Pull-Request: Object %llu requested by server %u — "
			"resolving local object and initiating transfer"),
		ObjectIdRaw, RequestingServerIdRaw);

	// Resolve the FRemoteObjectId to a local AActor and transfer it to
	// the requesting server. We construct an FRemoteObjectId from each actor
	// (which reads the remote ID from the object's internal handle) and match
	// against the requested ID.
	const FRemoteObjectId ObjectId = FRemoteObjectId::CreateFromInt(ObjectIdRaw);
	const FRemoteServerId RequestingServerId = FRemoteServerId::FromIdNumber(RequestingServerIdRaw);

	// Look up the local AActor in the world by matching its FRemoteObjectId.
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Pull-Request: No World — cannot resolve object %llu"),
			ObjectIdRaw);
		return;
	}

	AActor* FoundActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}
		// FRemoteObjectId(UObjectBase*) reads the remote ID from the object's handle
		const FRemoteObjectId ActorRemoteId(Actor);
		if (ActorRemoteId.IsValid() && ActorRemoteId == ObjectId)
		{
			FoundActor = Actor;
			break;
		}
	}

	if (!FoundActor)
	{
		UE_LOG(LogDSTMSub, Warning,
			TEXT("DSTM Pull-Request: Object %llu not found locally — "
				"it may have already been transferred or destroyed"),
			ObjectIdRaw);
		return;
	}

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Pull-Request: Resolved object %llu to %s — initiating transfer to server %u"),
		ObjectIdRaw, *FoundActor->GetName(), RequestingServerIdRaw);

	// Initiate the push transfer. This calls TransferObjectOwnershipToRemoteServer()
	// which serializes the actor, fires PostMigrate(Send), and invokes
	// RemoteObjectTransferDelegate → HandleOutgoingMigration() → beacon send.
	TransferActorToServer(FoundActor, RequestingServerId);
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE

// ─── Utility ─────────────────────────────────────────────────────

TArray<FString> UDSTMSubsystem::OffsetPeerPorts(
	const TArray<FString>& PeerAddresses, int32 Offset)
{
	TArray<FString> Result;
	Result.Reserve(PeerAddresses.Num());

	for (const FString& Addr : PeerAddresses)
	{
		// Parse "host:port" format
		FString Host;
		FString PortStr;
		if (Addr.Split(TEXT(":"), &Host, &PortStr, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			const int32 BasePort = FCString::Atoi(*PortStr);
			const int32 OffsetPort = BasePort + Offset;
			Result.Add(FString::Printf(TEXT("%s:%d"), *Host, OffsetPort));
		}
		else
		{
			// No port specified — cannot compute offset, peer will not be reachable
			UE_LOG(LogDSTMSub, Warning,
				TEXT("DSTM OffsetPeerPorts: Address '%s' has no port separator ':' — "
					"this peer will NOT have a DSTM beacon connection. "
					"Use 'host:port' format in -MultiServerPeers="),
				*Addr);
		}
	}

	return Result;
}

ADSTMBeaconClient* UDSTMSubsystem::FindBeaconForServer(uint32 ServerIdHash) const
{
	const FString* PeerId = ServerIdHashToPeerId.Find(ServerIdHash);
	if (!PeerId)
	{
		return nullptr;
	}

	const TObjectPtr<ADSTMBeaconClient>* BeaconPtr = PeerBeacons.Find(*PeerId);
	if (!BeaconPtr)
	{
		return nullptr;
	}

	ADSTMBeaconClient* Beacon = BeaconPtr->Get();
	if (!Beacon || !IsValid(Beacon))
	{
		// Peer beacon was destroyed (server disconnected/crashed).
		// Clean up stale map entries so future lookups don't hit dead references.
		UE_LOG(LogDSTMSub, Warning,
			TEXT("DSTM: Peer '%s' (hash %u) beacon is no longer valid — "
				"removing stale connection (peer likely disconnected or crashed)"),
			**PeerId, ServerIdHash);

		// const_cast is safe here: this is a lazy-cleanup pattern in a const lookup.
		// The alternative (periodic tick cleanup) would add unnecessary overhead.
		UDSTMSubsystem* MutableThis = const_cast<UDSTMSubsystem*>(this);
		FString PeerIdCopy = *PeerId;
		MutableThis->PeerBeacons.Remove(PeerIdCopy);
		MutableThis->ServerIdHashToPeerId.Remove(ServerIdHash);

		return nullptr;
	}

	return Beacon;
}
