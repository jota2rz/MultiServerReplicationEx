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
- [Beacon Mesh](#beacon-mesh)
- [GUID Seed (Not Needed with DSTM)](#guid-seed-not-needed-with-dstm)
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

The DSTM beacon mesh is a separate `UMultiServerNode` instance from any game-level multi-server mesh, keeping the transport concern isolated inside the plugin.

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
| `-DedicatedServerId=<string>` | Yes | Unique string identifier for this server (e.g. `server-1`). Hashed into a 10-bit `FRemoteServerId` in range [1, 1020] via `GetTypeHash() % 1020 + 1`. Also used as the beacon mesh `LocalPeerId` for peer identification. |
| `-DSTMListenPort=<int>` | Yes | Port for the DSTM beacon listener. Each server needs a unique port (per host). Defaults to `16000`. |
| `-DSTMListenIp=<ip>` | No | IP address to bind the DSTM beacon listener. Useful when servers should communicate over a private network interface separate from the game port. Defaults to `0.0.0.0`. |
| `-DSTMPeers=<ip:port,...>` | Yes (multi-server) | Comma-separated list of `host:port` pairs pointing to other servers' DSTM beacon ports. |

The expected server count for `AreAllPeersConnected()` is derived automatically as `PeerAddresses.Num() + 1` (peers + self). No separate count argument is needed.

> **Note: No GUID seed is needed.** With `UE_WITH_REMOTE_OBJECT_HANDLE=1`, every `FNetworkGUID` is derived from `FRemoteObjectId`, which embeds the 10-bit `ServerId` — collisions between servers are structurally impossible. The seed-based `FNetGUIDCache` counter (`NetworkGuidIndex`) is compile-time excluded by the DSTM code path.

### Example (two-server cluster)

```
# Server 1
-DedicatedServerId=server-1
-DSTMListenPort=16000
-DSTMPeers=127.0.0.1:16001

# Server 2
-DedicatedServerId=server-2
-DSTMListenPort=16001
-DSTMPeers=127.0.0.1:16000
```

With these arguments:
- Server 1 DSTM beacon listens on port **16000**
- Server 2 DSTM beacon listens on port **16001**
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
    16000,              // ListenPort
    Peers               // PeerAddresses
);
```

When providing peer addresses explicitly, supply the actual DSTM beacon ports.

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

## Beacon Mesh

The plugin creates its own `UMultiServerNode` for the DSTM transport. This is separate from any game-level multi-server mesh, keeping the transport concern fully inside the plugin.

The DSTM beacon listens on the port specified by `-DSTMListenPort=` (default `16000`). Peer addresses in `-DSTMPeers=` must point directly to each peer's DSTM beacon port.

When initializing explicitly (not via command-line), supply the DSTM ports directly in `PeerAddresses`.

### Server identity hashing

`FRemoteObjectId` packs a 10-bit `ServerId` field (valid range 1–1020) alongside a 53-bit serial number. The plugin derives the `FRemoteServerId` from a human-readable string using a bounded hash:

```cpp
// HashServerIdToRange(): (GetTypeHash(str) % 1020) + 1 → [1, 1020]
FRemoteServerId id = UDSTMSubsystem::GetRemoteServerIdFromString(TEXT("server-1"));
```

Both `InitializeServerIdentity()` (in the module) and `GetRemoteServerIdFromString()` / `HashServerIdToRange()` (in the subsystem) use the same formula. `HashServerIdToRange()` is a public static method for use in game code.

### Hash collision detection

Because the hash maps an arbitrary `FString` into only 1020 slots, two different server IDs could produce the same bounded hash. A collision would silently misroute migration data and, worse, make `FRemoteServerId::InitGlobalServerId()` assign duplicate identities to two different servers.

The plugin detects this at runtime: when a new peer connects, `HandlePeerConnected()` checks whether the computed hash already maps to a **different** peer ID. If a collision is detected, an `Error`-level log is emitted:

```
DSTM HASH COLLISION: DedicatedServerId 'zone-alpha' and 'zone-beta' both map to ServerId 42!
Migration routing will be BROKEN. Rename one of the server IDs.
```

For small clusters (< 20 servers), collisions are very unlikely. By the birthday paradox, collision probability reaches ~50% around 38 servers. For large deployments, test your naming scheme and monitor the log. If you encounter a collision, simply rename one of the colliding servers.

---

## GUID Seed (Not Needed with DSTM)

With `UE_WITH_REMOTE_OBJECT_HANDLE=1`, the engine derives every `FNetworkGUID` from `FRemoteObjectId` — a 64-bit value that embeds a 10-bit `ServerId` and a 53-bit `SerialNumber`. Because each server has a distinct `ServerId` baked into the ID, GUIDs from different servers are **structurally non-overlapping**. No manual seed is required.

The seed-based `FNetGUIDCache` counter (`NetworkGuidIndex`) that would normally need offsetting is compile-time **excluded** by the `#if UE_WITH_REMOTE_OBJECT_HANDLE` branches in `AssignNewNetGUID_Server()` and `AssignNewNetGUIDFromPath_Server()`.

### Legacy `ApplyGuidSeed()` API

The `ApplyGuidSeed(uint64)` method is still available as a public API for **non-DSTM** multi-server setups where servers share a proxy with overlapping GUID spaces. It is no longer called automatically from `InitializeFromCommandLine()` and the `-DSTMGuidSeed=` command-line argument has been removed.

---

## Runtime Scaling

The DSTM beacon mesh supports adding and removing servers at runtime. This enables dynamic auto-scaling, where an external orchestrator (e.g., Kubernetes, a custom matchmaker, or a monitoring service) manages the server pool while the game seamlessly handles player migration between any pair of connected servers.

### Adding a server at runtime

The MultiServer beacon host listens for incoming connections indefinitely after initialization. A new server can join the mesh at any time by including existing servers in its startup configuration:

```
# New server (server-3) starts with addresses of existing servers
-DedicatedServerId=server-3
-DSTMListenPort=16000
-DSTMPeers=192.168.1.10:16000,192.168.1.11:16000
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

The MultiServer Proxy (`UProxyNetDriver`) is **partially dynamic**. Its game server list is typically parsed from `-ProxyGameServers=` during `InitBase()`, but `RegisterGameServer()` is a public method that can be called at any time after initialization to add new game servers.

**What works without engine changes:**

- `RegisterGameServer(const FURL&)` can be called post-init — it appends to the `GameServerConnections` array
- Clients that connect **after** a dynamic registration automatically get routes to the new server (the proxy iterates the full `GameServerConnections` array on each `NMT_Join`)
- `UProxyNetDriver` is exported (`MULTISERVERREPLICATION_API`) and can be subclassed by plugins

**What does not work (engine limitations):**

- **Existing clients** are not routed to dynamically added servers — `ConnectToGameServer()` is private to `UProxyListenerNotify` and only runs at client join time
- There is **no `UnregisterGameServer()`** — removing a game server at runtime has no API (no route cleanup, no backend driver teardown)
- There is **no game server crash detection** — the proxy relies on standard `UIpNetDriver` UDP timeouts with no dedicated callback or reconnection logic
- If a game server crashes, clients routed to it will eventually be disconnected when the connection times out

Fixing these gaps (retroactive routing for existing clients, server removal, crash hooks) would require engine modifications to `UProxyNetDriver` and `UProxyListenerNotify`.

### Orchestration notes

The automation of scaling decisions is **out of scope** for this plugin. An external application should handle:
- When to spin up / tear down server instances
- Assigning unique `-DedicatedServerId=` values
- Providing the correct `-DSTMPeers=` addresses to new servers
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
- Both servers are running and have received `-DSTMPeers=` pointing to each other's DSTM ports
- `AreAllPeersConnected()` returns `true` before initiating the first migration
- Firewall rules allow traffic on the DSTM beacon port

### Double-transfer: PlayerController and Pawn both passed separately

`TransferObjectOwnershipToRemoteServer()` serializes the passed actor **and all its subobjects**, including the possessed `Pawn`. Passing the `Pawn` to `TransferActorToServer()` in addition to the `PlayerController` causes two migration payloads and results in undefined behavior on the destination server. Pass only the `PlayerController`.

### `Failed to deserialize FRemoteObjectData` on receive

A serialization version mismatch between the two servers. Both server binaries must be built from the same source. This error can also occur if the network payload was truncated—ensure the beacon allows unlimited bunch sizes (`SetUnlimitedBunchSizeAllowed(true)` is inherited from `AMultiServerBeaconClient`).

### DSTM mesh created but `AreAllPeersConnected()` never returns `true`

`AreAllPeersConnected()` is a startup readiness check: it waits until all peers listed in `-DSTMPeers=` have connected and exchanged IDs. If the peer list is correct but the check never passes, verify network connectivity and that each peer's beacon listener port is reachable.

**In dynamic meshes** where servers join and leave, `AreAllPeersConnected()` becomes unreliable after a server departure — `NumExpectedServers` never decreases, so the check stays `false` permanently. Use `GetConnectedPeerCount() > 0` or `GetConnectedPeerIds()` instead. See [Runtime Scaling](#runtime-scaling).

### `Reassigning NetGUID` warnings / `ObjectReplicatorReceivedBunchFail` crashes

If you see these errors despite using DSTM with `UE_WITH_REMOTE_OBJECT_HANDLE=1`, check that every server has a unique `-DedicatedServerId=`. Duplicate server IDs produce duplicate `FRemoteServerId` values, which can cause identical `FRemoteObjectId` and thus identical `FNetworkGUID` values. Also verify that no hash collision exists (check the log for `DSTM HASH COLLISION` messages).
