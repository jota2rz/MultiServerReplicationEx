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

This plugin requires **one engine source modification**: setting `UE_WITH_REMOTE_OBJECT_HANDLE` to `1` in `Engine/Source/Runtime/Core/Public/Misc/CoreMiscDefines.h`. The stock UE 5.7 default is `0`.

```cpp
// CoreMiscDefines.h — change required
#ifndef UE_WITH_REMOTE_OBJECT_HANDLE
    #define UE_WITH_REMOTE_OBJECT_HANDLE 1   // stock default is 0
#endif
```

This is the **only** engine change needed. No other engine source files are modified.

If the define is `0`, the plugin compiles but remains inert: the module logs a warning, skips delegate binding, and the subsystem reports `IsMeshActive() == false`.

> **Important:** Setting `UE_WITH_REMOTE_OBJECT_HANDLE=1` changes `FObjectHandle` from a simple pointer to `FRemoteObjectHandlePrivate` (tagged pointer union). Every `TObjectPtr<>` in the engine changes ABI. All modules (engine, plugins, game) must be compiled against the same setting. A pre-built/installed engine cannot be mixed with a source-built one. You must build the engine from source with this change.

---

## Command-Line Arguments

Each server process that participates in the DSTM mesh must receive these arguments:

| Argument | Required | Description |
|----------|----------|-------------|
| `-DedicatedServerId=<string>` | Yes | Unique string identifier for this server (e.g. `server-1`). Hashed to a `uint32` via `GetTypeHash()` to produce the `FRemoteServerId`. Also used as the beacon mesh `LocalPeerId` for peer identification. |
| `-MultiServerListenPort=<int>` | Yes | Base port for the main MultiServer mesh. The DSTM mesh listens on this port + 1000. |
| `-MultiServerListenIp=<ip>` | No | IP address to bind the DSTM beacon listener. Defaults to `0.0.0.0`. |
| `-MultiServerPeers=<ip:port,...>` | Yes (multi-server) | Comma-separated list of `host:port` pairs for other servers' **main** MultiServer mesh ports. The plugin automatically adds +1000 to each port for the DSTM mesh. |
| `-DSTMGuidSeed=<uint64>` | Recommended | GUID allocation seed for the server's `FNetGUIDCache`. Each server must use a distinct value (e.g. `100000`, `200000`) to prevent `FNetworkGUID` collisions in the proxy's shared backend cache. See [GUID Seed](#guid-seed). |

The expected server count for `AreAllPeersConnected()` is derived automatically as `PeerAddresses.Num() + 1` (peers + self). No separate count argument is needed.

### Example (two-server cluster)

```
# Server 1
-DedicatedServerId=server-1
-MultiServerListenPort=15000
-MultiServerPeers=127.0.0.1:15001
-DSTMGuidSeed=100000

# Server 2
-DedicatedServerId=server-2
-MultiServerListenPort=15001
-MultiServerPeers=127.0.0.1:15000
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

`InitializeFromCommandLine()` reads the command-line arguments described above, computes the DSTM port, and calls `InitializeDSTMMesh()` internally. It returns `true` if the mesh was created or `false` if the process is not in multi-server mode (no `-DedicatedServerId=` present).

### Explicit initialization (without command-line args)

```cpp
UDSTMSubsystem* DSTM = GI->GetSubsystem<UDSTMSubsystem>();

TArray<FString> Peers = { TEXT("192.168.1.20:16001") };
DSTM->InitializeDSTMMesh(
    TEXT("server-1"),   // LocalPeerId
    TEXT("0.0.0.0"),    // ListenIp
    16000,              // ListenPort  (already offset)
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

### Hash collision detection

Because `GetTypeHash()` maps an arbitrary `FString` to a `uint32`, two different server IDs could theoretically produce the same hash. A collision would silently misroute migration data to the wrong server.

The plugin detects this at runtime: when a new peer connects, `HandlePeerConnected()` checks whether the computed hash already maps to a **different** peer ID. If a collision is detected, an `Error`-level log is emitted:

```
DSTM HASH COLLISION: DedicatedServerId 'zone-alpha' and 'zone-beta' both hash to 1234567890!
Migration routing will be BROKEN. Rename one of the server IDs.
```

In practice, collisions are extremely unlikely for typical server names (`server-1`, `server-2`, `zone-west`, etc.). If you encounter one, simply rename one of the colliding servers.

---

## GUID Seed

### Why it's needed

In a multi-server topology with a shared proxy, each backend server allocates `FNetworkGUID` values sequentially starting from the same counter. When Server-1 spawns a `PlayerController` (gets GUID 4) and Server-2 also spawns one (also gets GUID 4), the proxy's shared backend `FNetGUIDCache` encounters a collision — it reassigns the GUID mapping, corrupting replication and potentially crashing.

This is a **separate concern from DSTM**. DSTM uses `FRemoteObjectId` for cross-server object identity during migration. The GUID seed prevents proxy-level `FNetworkGUID` collisions during normal replication, which are equally problematic with or without DSTM.

### How it works

The plugin replaces the `NetDriver`'s `GuidCache` with a new `FNetGUIDCache` initialized with the specified seed via the `GetNetGuidCache()` accessor (avoiding direct member access to the deprecated `GuidCache` field). The seed offsets the GUID counter so that servers allocate from disjoint ranges:

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

The engine's built-in `-NetworkGuidSeed=` parameter is gated by `#if !UE_BUILD_SHIPPING` and doesn't work in Shipping builds. The plugin's `ApplyGuidSeed()` uses the `GetNetGuidCache()` accessor (which returns a mutable `TSharedPtr<FNetGUIDCache>&`) and the public `ENGINE_API` constructor, so it works in **all build configurations** including Shipping and is forward-compatible with Epic's planned deprecation of the `GuidCache` member variable.

---

## Runtime Scaling

The DSTM beacon mesh supports adding and removing servers at runtime. This enables dynamic auto-scaling, where an external orchestrator (e.g., Kubernetes, a custom matchmaker, or a monitoring service) manages the server pool while the game seamlessly handles player migration between any pair of connected servers.

### Adding a server at runtime

The MultiServer beacon host listens for incoming connections indefinitely after initialization. A new server can join the mesh at any time by including existing servers in its startup configuration:

```
# New server (server-3) starts with addresses of existing servers
-DedicatedServerId=server-3
-MultiServerListenPort=15000
-MultiServerPeers=192.168.1.10:15000,192.168.1.11:15000
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

### Server rejoin (crash recovery)

If a server crashes and restarts with the same `-DedicatedServerId`, it can reconnect to the mesh by initiating outbound connections to existing servers. `HandlePeerConnected()` detects the returning peer ID, unbinds delegates from the old (now-destroyed) beacon, and binds to the new one. The routing tables are updated in place — no stale state accumulates.

### `AreAllPeersConnected()` caveats

`AreAllPeersConnected()` delegates to `UMultiServerNode::AreAllServersConnected()`, which checks:

```
NumAcknowledgedPeers >= (NumExpectedServers - 1)
```

where `NumExpectedServers` is derived **once** from `PeerAddresses.Num() + 1` at mesh creation time and never updated. This has implications for dynamic meshes:

| Scenario | `AreAllPeersConnected()` behavior |
|----------|-----------------------------------|
| All original peers connected | Returns `true` (normal) |
| Extra server joins (wasn't counted) | Still returns `true` — uses `>=` |
| Server leaves (crash/shutdown) | Returns `false` and **stays false** — `NumExpectedServers` never decreases |
| Server leaves then rejoins | Returns `true` again once peer count is restored |

**For dynamic meshes, prefer `GetConnectedPeerCount()` and `GetConnectedPeerIds()` over `AreAllPeersConnected()`.** These methods reflect the actual live peer set rather than comparing against a fixed count.

```cpp
UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();

// Dynamic mesh: use peer count instead of AreAllPeersConnected()
int32 PeerCount = DSTM->GetConnectedPeerCount();
if (PeerCount > 0)
{
    // At least one peer is available for migration
}

// Or check for a specific peer
TArray<FString> PeerIds = DSTM->GetConnectedPeerIds();
if (PeerIds.Contains(TEXT("server-2")))
{
    // server-2 is connected and ready
}
```

### Monitoring peer status

```cpp
UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();

// Fixed mesh: check if initial peers are connected
bool bReady = DSTM->AreAllPeersConnected();

// Dynamic mesh: get current peer count (valid/connected only)
int32 PeerCount = DSTM->GetConnectedPeerCount();

// Get the IDs of all currently connected peers
TArray<FString> PeerIds = DSTM->GetConnectedPeerIds();
```

### MultiServer Proxy limitations

The MultiServer Proxy (`UProxyNetDriver`) is **not dynamic**. Its game server list is parsed from `-ProxyGameServers=` at initialization and cannot be changed at runtime. There is no engine API to register or unregister game servers after `InitBase()`. This means:

- All backend game servers must be listed in the proxy's startup command line
- Adding a new game server requires restarting the proxy
- If a game server crashes, clients routed to it will be disconnected (the proxy detects the closed connection and cleans up routes)

This is an engine-level limitation in UE 5.7's `UProxyNetDriver`. Dynamic proxy scaling would require Epic to add runtime `RegisterGameServer()` / `UnregisterGameServer()` support.

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

Your engine build does not have DSTM support compiled in. The plugin requires a custom UE 5.7 build with `UE_WITH_REMOTE_OBJECT_HANDLE=1` defined in `Engine\Source\Runtime\Core\Public\Misc\CoreMiscDefines.h` or the build environment.

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

`AreAllPeersConnected()` is a startup readiness check: it waits until all peers listed in `-MultiServerPeers=` have connected and exchanged IDs. If the peer list is correct but the check never passes, verify network connectivity and that each peer's beacon listener port is reachable.

**In dynamic meshes** where servers join and leave, `AreAllPeersConnected()` becomes unreliable after a server departure — `NumExpectedServers` never decreases, so the check stays `false` permanently. Use `GetConnectedPeerCount() > 0` or `GetConnectedPeerIds()` instead. See [Runtime Scaling](#runtime-scaling).

### `Reassigning NetGUID` warnings / `ObjectReplicatorReceivedBunchFail` crashes

GUID collisions between backend servers. Each server must use a distinct `-DSTMGuidSeed=` value so that independently spawned actors (PlayerControllers, PlayerStates, Pawns, NoPawnPlayerControllers) get non-overlapping GUIDs. Without this, Server-1's GUID 4 → PlayerController and Server-2's GUID 4 → NoPawnPlayerController collide in the proxy's shared `FNetGUIDCache`, causing replication data to be applied to the wrong object type. See [GUID Seed](#guid-seed).
