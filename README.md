# DSTMTransport Plugin

A plugin for Unreal Engine 5.7 that completes the engine's built-in DSTM (Distributed State Transfer Machine) framework for seamless cross-server actor migration. It replaces the default disk-based transport with a real-time beacon mesh, enabling servers to push serialized actors directly to each other over the network without the client disconnecting.

## Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Architecture](#architecture)
- [Adding the Plugin to Your Project](#adding-the-plugin-to-your-project)
- [Engine Build Requirement](#engine-build-requirement)
- [Command-Line Arguments](#command-line-arguments)
- [Initialization](#initialization)
- [Migrating an Actor](#migrating-an-actor)
- [Migration Flow Reference](#migration-flow-reference)
- [Pull Migration](#pull-migration)
- [Beacon Mesh and Port Offset](#beacon-mesh-and-port-offset)
- [GUID Seed](#guid-seed)
- [Runtime Scaling](#runtime-scaling)
- [Logging](#logging)
- [Troubleshooting](#troubleshooting)

---

## Overview

Unreal Engine 5.7 ships a DSTM framework (`UE::RemoteObject::Transfer`) that can serialize any actor—including its player controller, possessed pawn, and all subobjects—and reconstitute it on another server without the client noticing a disconnect. By default, the engine expects a disk- or platform-specific transport layer to move the serialized blob between servers. **DSTMTransport** provides that transport layer on top of the `MultiServerReplication` plugin's beacon mesh.

The plugin consists of three cooperating classes:

| Class | Responsibility |
|-------|---------------|
| `FDSTMTransportModule` | Module startup: initializes the server's `FRemoteServerId` and pre-binds the engine transport delegates |
| `UDSTMSubsystem` | Runtime: manages the DSTM beacon mesh, routes outgoing and incoming migration data, handles pull-requests |
| `ADSTMBeaconClient` | Network: extends `AMultiServerBeaconClient` with reliable RPCs that carry serialized `FRemoteObjectData` |

---

## Prerequisites

- Unreal Engine 5.7 (custom build with `UE_WITH_REMOTE_OBJECT_HANDLE=1`)
- `MultiServerReplication` plugin (ships with UE 5.7)
- A dedicated-server topology where each server process has a unique string ID

---

## Architecture

```
Server-A                              Beacon Mesh              Server-B
────────                              ───────────              ────────
TransferActorToServer(PC)
  └─► TransferObjectOwnership
        ToRemoteServer()
            │
            ▼
  RemoteObjectTransferDelegate
  (bound by DSTMTransportModule)
            │
            ▼
  HandleOutgoingMigration()
  [FMemoryWriter → TArray<u8>]
            │
            └──── beacon RPC ────►
                                      ServerReceive/           Beacon receives
                                      ClientReceive            migration data
                                      MigratedObject()               │
                                                                     ▼
                                                         HandleIncomingMigrationData()
                                                         [FMemoryReader ← TArray<u8>]
                                                                     │
                                                                     ▼
                                                         OnObjectDataReceived()
                                                         [engine DSTM receive pipeline]
                                                                     │
                                                                     ▼
                                                         AActor::PostMigrate(Receive)
                                                         APlayerController::PostMigrate(Receive)
```

The DSTM beacon mesh is a separate `UMultiServerNode` instance from any game-level multi-server mesh. It listens on a port offset (+1000 by default) from the main MultiServer mesh, keeping the transport concern isolated inside the plugin.

---

## Adding the Plugin to Your Project

1. Copy the `Plugins/DSTMTransport` folder into your project's `Plugins/` directory.

2. Add `DSTMTransport` to the plugins list in your `.uproject` file:

   ```json
   {
     "Plugins": [
       { "Name": "MultiServerReplication", "Enabled": true },
       { "Name": "DSTMTransport", "Enabled": true }
     ]
   }
   ```

3. Add `DSTMTransport` to your game module's `PublicDependencyModuleNames` if you call subsystem methods directly, or `PrivateDependencyModuleNames` if you only call through the game instance:

   ```cs
   // YourGame.Build.cs
   PrivateDependencyModuleNames.AddRange(new string[]
   {
       "DSTMTransport",
   });
   ```

4. Rebuild your project.

---

## Engine Build Requirement

The plugin requires an engine build where `UE_WITH_REMOTE_OBJECT_HANDLE` is set to `1`. In the UE 5.7 source repository this defaults to `1` (see `CoreMiscDefines.h`), but the pre-built/installed engine may have it set to `0`. Verify by checking `Engine/Source/Runtime/Core/Public/Misc/CoreMiscDefines.h` for the define value.

If the define is `0`, the plugin compiles but remains inert: the module logs a warning, skips delegate binding, and the subsystem reports `IsMeshActive() == false`.

> **Important:** Setting `UE_WITH_REMOTE_OBJECT_HANDLE=1` changes `FObjectHandle` from a simple pointer to `FRemoteObjectHandlePrivate` (tagged pointer union). Every `TObjectPtr<>` in the engine changes ABI. All modules (engine, plugins, game) must be compiled against the same setting. A pre-built/installed engine cannot be mixed with a source-built one.

---

## Command-Line Arguments

Each server process that participates in the DSTM mesh must receive these arguments:

| Argument | Required | Description |
|----------|----------|-------------|
| `-DedicatedServerId=<string>` | Yes | Unique string identifier for this server (e.g. `server-1`). Hashed to a `uint32` via `GetTypeHash()` to produce the `FRemoteServerId`. Must be the same string used by your game code to identify this server. |
| `-MultiServerLocalId=<string>` | Yes | Peer ID used in the MultiServer beacon mesh. Should match `-DedicatedServerId=` for correct peer routing. |
| `-MultiServerListenPort=<int>` | Yes | Base port for the main MultiServer mesh. The DSTM mesh listens on this port + 1000. |
| `-MultiServerListenIp=<ip>` | No | IP address to bind the DSTM beacon listener. Defaults to `0.0.0.0`. |
| `-MultiServerPeers=<ip:port,...>` | Yes (multi-server) | Comma-separated list of `host:port` pairs for other servers' **main** MultiServer mesh ports. The plugin automatically adds +1000 to each port for the DSTM mesh. |
| `-MultiServerNumServers=<int>` | No | Total number of servers in the cluster. Used to determine when `AreAllPeersConnected()` returns true. |
| `-DSTMGuidSeed=<uint64>` | Recommended | GUID allocation seed for the server's `FNetGUIDCache`. Each server must use a distinct value (e.g. `100000`, `200000`) to prevent `FNetworkGUID` collisions in the proxy's shared backend cache. See [GUID Seed](#guid-seed). |

### Example (two-server cluster)

```
# Server 1
-DedicatedServerId=server-1
-MultiServerLocalId=server-1
-MultiServerListenPort=15000
-MultiServerPeers=127.0.0.1:15001
-MultiServerNumServers=2
-DSTMGuidSeed=100000

# Server 2
-DedicatedServerId=server-2
-MultiServerLocalId=server-2
-MultiServerListenPort=15001
-MultiServerPeers=127.0.0.1:15000
-MultiServerNumServers=2
-DSTMGuidSeed=200000
```

With these arguments:
- Server 1 DSTM beacon listens on port **16000** (15000 + 1000)
- Server 2 DSTM beacon listens on port **16001** (15001 + 1000)
- Each server connects its DSTM beacon to the other's DSTM port

---

## Initialization

### Why `Initialize()` does not auto-start the mesh

`UGameInstanceSubsystem::Initialize()` runs during `GameInstance` creation, before any `UWorld` exists. `GetWorld()` returns `nullptr` at that point. Creating a beacon mesh requires a valid world, so the subsystem starts inert and waits to be told when to initialize.

### Calling from your Game Mode

Call `InitializeFromCommandLine()` from your game mode's `StartPlay()` (or equivalent) once the world is ready:

```cpp
// YourGameMode.cpp
#include "DSTMSubsystem.h"

void AYourGameMode::StartPlay()
{
    Super::StartPlay();

    if (UGameInstance* GI = GetGameInstance())
    {
        if (UDSTMSubsystem* DSTM = GI->GetSubsystem<UDSTMSubsystem>())
        {
            DSTM->InitializeFromCommandLine();
        }
    }
}
```

`InitializeFromCommandLine()` reads the command-line arguments described above, computes the DSTM port, and calls `InitializeDSTMMesh()` internally. It returns `true` if the mesh was created or `false` if the process is not in multi-server mode (no `-MultiServerLocalId=` present).

### Explicit initialization (without command-line args)

```cpp
UDSTMSubsystem* DSTM = GI->GetSubsystem<UDSTMSubsystem>();

TArray<FString> Peers = { TEXT("192.168.1.20:16001") };
DSTM->InitializeDSTMMesh(
    TEXT("server-1"),   // LocalPeerId
    TEXT("0.0.0.0"),    // ListenIp
    16000,              // ListenPort  (already offset)
    2,                  // NumServers
    Peers               // PeerAddresses (already offset)
);
```

When providing peer addresses explicitly, include the DSTM port offset yourself (i.e. the actual DSTM port, not the base MultiServer port).

### Checking readiness

```cpp
if (DSTM->IsMeshActive() && DSTM->AreAllPeersConnected())
{
    // Safe to call TransferActorToServer()
}
```

`IsMeshActive()` — returns `true` once `InitializeDSTMMesh()` succeeds.  
`AreAllPeersConnected()` — returns `true` when every expected peer has an established beacon connection.

---

## Migrating an Actor

```cpp
// Get the subsystem
UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();

// Resolve the destination server's FRemoteServerId from its string ID
FRemoteServerId DestId = UDSTMSubsystem::GetRemoteServerIdFromString(TEXT("server-2"));

// Transfer the actor — serializes PC + all subobjects including possessed Pawn.
// Do NOT call this separately for the Pawn; it is included automatically.
DSTM->TransferActorToServer(PlayerController, DestId);
```

`TransferActorToServer()` calls `UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer()`, which:

1. Serializes the actor and all its subobjects into `FRemoteObjectData`
2. Calls `AActor::PostMigrate(Send)` — removes the actor from the world, closes the replication channel with the `Migrated` flag
3. For player controllers: calls `APlayerController::PostMigrate(Send)` — swaps in a `NoPawnPC`, saves the connection handle
4. Invokes `RemoteObjectTransferDelegate` → `HandleOutgoingMigration()` → sends via beacon RPC

> **Important:** Only pass the `PlayerController`. The possessed `Pawn` is automatically included as a subobject. Passing both separately causes a double-transfer and will corrupt the migration.

### Convenience: first connected peer

For a two-server setup where there is exactly one peer:

```cpp
FRemoteServerId PeerId;
if (DSTM->GetFirstPeerServerId(PeerId))
{
    DSTM->TransferActorToServer(PlayerController, PeerId);
}
```

---

## Migration Flow Reference

### Push (source server sends the actor)

```
Source server calls TransferActorToServer(Actor, DestServerId)
  │
  ├─ Engine: TransferObjectOwnershipToRemoteServer()
  │    ├─ Serialize Actor + subobjects → FRemoteObjectData
  │    └─ Call RemoteObjectTransferDelegate
  │
  └─ DSTMTransportModule::OnRemoteObjectTransfer()
       └─ UDSTMSubsystem::HandleOutgoingMigration()
            ├─ Serialize FRemoteObjectData → TArray<uint8> via FMemoryWriter
            ├─ Look up ADSTMBeaconClient for DestServerId
            └─ Send via RPC:
                 HasAuthority() == true  → ClientReceiveMigratedObject()
                 HasAuthority() == false → ServerReceiveMigratedObject()

Destination server receives RPC
  └─ ADSTMBeaconClient fires OnMigrationDataReceived
       └─ UDSTMSubsystem::HandleIncomingMigrationData()
            ├─ Deserialize TArray<uint8> → FRemoteObjectData via FMemoryReader
            └─ UE::RemoteObject::Transfer::OnObjectDataReceived()
                 ├─ Deserialize Actor + subobjects into existing C++ object
                 ├─ AActor::PostMigrate(Receive) → add to world, begin replication
                 └─ APlayerController::PostMigrate(Receive) → bind to connection
```

### RPC direction

Each server-to-server beacon connection has one side with authority (`HasAuthority() == true`) and one without. The plugin checks authority at send time to select the correct RPC direction so that the UE networking stack accepts the call:

- Server side of beacon → sends via **Client RPC** to reach the other process
- Client side of beacon → sends via **Server RPC** to reach the other process

This applies identically to both data-transfer RPCs and pull-request RPCs.

---

## Pull Migration

A "pull" migration happens when a destination server requests an object that still lives on another server—for example, when the engine's DSTM scheduler determines that an object should move before the source server has initiated it.

The engine calls `RequestRemoteObjectDelegate` on the destination server. The plugin handles this with `HandleObjectRequest()`:

1. Looks up the beacon for `LastKnownServerId`
2. Sends `ServerRequestMigrateObject()` or `ClientRequestMigrateObject()` depending on beacon authority

The source server receives the request, fires `OnMigrationRequested`, and `HandleIncomingMigrationRequest()`:

1. Iterates all world actors, matching `FRemoteObjectHandle.GetRemoteObjectId()` against the requested `FRemoteObjectId`
2. Calls `TransferActorToServer(FoundActor, RequestingServerId)` — which triggers the normal push flow

---

## Beacon Mesh and Port Offset

The plugin creates a dedicated `UMultiServerNode` separate from any game-level multi-server mesh. This keeps the DSTM transport concern fully inside the plugin.

The DSTM mesh port is computed as:

```
DSTMListenPort = MultiServerListenPort + DSTMPortOffset
```

where `DSTMPortOffset = 1000` (a compile-time constant in `UDSTMSubsystem`).

Peer addresses supplied via `-MultiServerPeers=` use the **base** MultiServer port. The plugin rewrites them automatically by adding `DSTMPortOffset` to each port before creating the mesh.

If you initialize the mesh explicitly (not via command-line), supply the already-offset DSTM ports in `PeerAddresses`.

### Server identity hashing

`FRemoteServerId` is a `uint32`. The plugin derives it from a human-readable string (`DedicatedServerId`) using `GetTypeHash(FString)`:

```cpp
FRemoteServerId id = FRemoteServerId::FromIdNumber(GetTypeHash(TEXT("server-1")));
```

`GetRemoteServerIdFromString()` performs this hash publicly so your game code can produce the same value when specifying migration targets.

---

## GUID Seed

### Why it's needed

In a multi-server topology with a shared proxy, each backend server allocates `FNetworkGUID` values sequentially starting from the same counter. When Server-1 spawns a `PlayerController` (gets GUID 4) and Server-2 also spawns one (also gets GUID 4), the proxy's shared backend `FNetGUIDCache` encounters a collision — it reassigns the GUID mapping, corrupting replication and potentially crashing.

This is a **separate concern from DSTM**. DSTM uses `FRemoteObjectId` for cross-server object identity during migration. The GUID seed prevents proxy-level `FNetworkGUID` collisions during normal replication, which are equally problematic with or without DSTM.

### How it works

The plugin replaces the `NetDriver`'s `GuidCache` with a new `FNetGUIDCache` initialized with the specified seed. The seed offsets the GUID counter so that servers allocate from disjoint ranges:

| Server | Seed | GUID range |
|--------|------|-----------|
| server-1 | 100000 | 100001, 100002, 100003, ... |
| server-2 | 200000 | 200001, 200002, 200003, ... |
| server-3 | 300000 | 300001, 300002, 300003, ... |

### Command-line usage

```
-DSTMGuidSeed=100000   # Server 1
-DSTMGuidSeed=200000   # Server 2
```

The `InitializeFromCommandLine()` method reads this argument and calls `ApplyGuidSeed()` automatically.

### Programmatic usage

```cpp
UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();
DSTM->ApplyGuidSeed(100000);  // Call before any clients connect
```

### Shipping builds

The engine's built-in `-NetworkGuidSeed=` parameter is gated by `#if !UE_BUILD_SHIPPING` and doesn't work in Shipping builds. The plugin's `ApplyGuidSeed()` replaces the `GuidCache` directly via the public `ENGINE_API` constructor, so it works in **all build configurations** including Shipping.

---

## Runtime Scaling

The DSTM beacon mesh supports adding and removing servers at runtime. This enables dynamic auto-scaling, where an external orchestrator (e.g., Kubernetes, a custom matchmaker, or a monitoring service) manages the server pool while the game seamlessly handles player migration between any pair of connected servers.

### Adding a server at runtime

The MultiServer beacon host listens for incoming connections indefinitely after initialization. A new server can join the mesh at any time by including existing servers in its startup configuration:

```
# New server (server-3) starts with addresses of existing servers
-DedicatedServerId=server-3
-MultiServerLocalId=server-3
-MultiServerListenPort=15000
-MultiServerPeers=192.168.1.10:15000,192.168.1.11:15000
-MultiServerNumServers=3
-DSTMGuidSeed=300000
```

**Flow:**
1. The new server's `UMultiServerNode::Create()` opens outbound beacon connections to existing servers
2. Existing servers' beacon hosts accept the connections automatically (no reconfiguration needed)
3. Both sides fire `OnMultiServerConnected` → `HandlePeerConnected()` registers the new peer
4. Migration RPCs can flow between the new server and all existing servers immediately

**Existing servers do not need to be reconfigured or restarted.** Their beacon hosts accept new inbound connections from any server that connects. The `HandlePeerConnected` callback correctly registers new peers in the routing tables at runtime.

> **Engine limitation:** The UE 5.7 `UMultiServerNode` API does not expose a public method to proactively connect to a new server from an already-running instance. New servers must always initiate the connection (inbound to existing hosts). This means the new server must know at least one existing server's address at startup.

### Removing a server at runtime

When a server shuts down or crashes:
1. The UE beacon system detects the disconnection
2. The `TObjectPtr` to the peer's `ADSTMBeaconClient` becomes invalid
3. On the next migration attempt to the disconnected server, `FindBeaconForServer()` detects the invalid beacon, logs a warning, and cleans up stale entries from the routing tables
4. The error is logged: `"Peer 'server-X' beacon is no longer valid — removing stale connection"`

Migration attempts to a disconnected server will fail gracefully with a `"No beacon connection to destination server"` error. The remaining mesh continues to function normally for all other connected peers.

### Monitoring peer status

```cpp
UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();

// Check if initial peers are connected
bool bReady = DSTM->AreAllPeersConnected();

// Get current peer count (valid/connected only)
int32 PeerCount = DSTM->GetConnectedPeerCount();

// Get the IDs of all currently connected peers
TArray<FString> PeerIds = DSTM->GetConnectedPeerIds();
```

### Orchestration notes

The automation of scaling decisions is **out of scope** for this plugin. An external application should handle:
- When to spin up / tear down server instances
- Assigning unique `-DedicatedServerId=` and `-DSTMGuidSeed=` values
- Providing the correct `-MultiServerPeers=` addresses to new servers
- Deciding which server to migrate players *to* before shutting down a server

---

## Logging

| Log category | Used in |
|---|---|
| `LogDSTM` | `FDSTMTransportModule` — module startup, delegate binding, server identity |
| `LogDSTMSub` | `UDSTMSubsystem` — mesh lifecycle, peer connections, migration send/receive |

Enable verbose output:

```ini
; DefaultEngine.ini
[Core.Log]
LogDSTM=Verbose
LogDSTMSub=Verbose
```

---

## Troubleshooting

### `UE_WITH_REMOTE_OBJECT_HANDLE is disabled` warning at startup

Your engine build does not have DSTM support compiled in. The plugin requires a custom UE 5.7 build with `UE_WITH_REMOTE_OBJECT_HANDLE=1` defined in `RemoteObjectHandle.h` or the build environment.

### `No -DedicatedServerId= on command line` — migration never starts

Each server process must receive `-DedicatedServerId=<unique-string>`. Without it, `FRemoteServerId::InitGlobalServerId()` is never called, delegate binding is skipped, and the engine has no server identity for routing.

### `No beacon connection to destination server N! Migration data lost`

The DSTM beacon mesh has not finished connecting to the target server. Ensure:
- Both servers are running and have received `-MultiServerPeers=` pointing to each other's **base** ports
- `AreAllPeersConnected()` returns `true` before initiating the first migration
- Firewall rules allow traffic on both the base port and the base port + 1000

### Double-transfer: PlayerController and Pawn both passed separately

`TransferObjectOwnershipToRemoteServer()` serializes the passed actor **and all its subobjects**, including the possessed `Pawn`. Passing the `Pawn` to `TransferActorToServer()` in addition to the `PlayerController` causes two migration payloads and results in undefined behavior on the destination server. Pass only the `PlayerController`.

### `Failed to deserialize FRemoteObjectData` on receive

A serialization version mismatch between the two servers. Both server binaries must be built from the same source. This error can also occur if the network payload was truncated—ensure the beacon allows unlimited bunch sizes (`SetUnlimitedBunchSizeAllowed(true)` is inherited from `AMultiServerBeaconClient`).

### DSTM mesh created but `AreAllPeersConnected()` never returns `true`

Verify that `-MultiServerNumServers=` matches the actual number of servers minus one (or the number of distinct peers each server should connect to). If you omit this argument, the default is `1`, so a two-server cluster will report all peers connected as soon as one peer connects—even if additional peers are expected.

### `Reassigning NetGUID` warnings / `ObjectReplicatorReceivedBunchFail` crashes

GUID collisions between backend servers. Each server must use a distinct `-DSTMGuidSeed=` value so that independently spawned actors (PlayerControllers, PlayerStates, Pawns, NoPawnPlayerControllers) get non-overlapping GUIDs. Without this, Server-1's GUID 4 → PlayerController and Server-2's GUID 4 → NoPawnPlayerController collide in the proxy's shared `FNetGUIDCache`, causing replication data to be applied to the wrong object type. See [GUID Seed](#guid-seed).
