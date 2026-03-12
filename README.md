# MultiServerReplicationEx

Extended fork of UE 5.7's `MultiServerReplication` engine plugin with **dynamic proxy support** and integrated **DSTM transport** for seamless cross-server player migration via beacon mesh.

This plugin combines two concerns into a single module:

1. **Proxy fixes** — adds and removes game servers at runtime, detects game server crashes, HTTP-based dynamic registration
2. **DSTM transport** — replaces the engine's default disk-based migration transport with a real-time beacon mesh, enabling servers to push serialized actors directly to each other over the network without the client disconnecting

## Contents

- [Overview](#overview)
- [Changes from Stock MultiServerReplication](#changes-from-stock-multiserverreplication)
- [Prerequisites](#prerequisites)
- [Adding the Plugin to Your Project](#adding-the-plugin-to-your-project)
- [Engine Build Requirement](#engine-build-requirement)
- [Architecture](#architecture)
- [Command-Line Arguments](#command-line-arguments)
- [Initialization](#initialization)
- [Migrating an Actor](#migrating-an-actor)
- [Pawn Handling During Migration](#pawn-handling-during-migration)
- [Migration Flow Reference](#migration-flow-reference)
- [Pull Migration](#pull-migration)
- [Beacon Mesh](#beacon-mesh)
- [GUID Seed (Not Needed with DSTM)](#guid-seed-not-needed-with-dstm)
- [Runtime Scaling](#runtime-scaling)
- [Dynamic Proxy](#dynamic-proxy)
- [Dynamic Proxy Registration (HTTP)](#dynamic-proxy-registration-http)
- [Tip: Seamless Transfer with a Custom CMC](#tip-seamless-transfer-with-a-custom-cmc)
- [Logging](#logging)
- [Troubleshooting](#troubleshooting)
- [Known Issues & Required Engine Patches](#known-issues--required-engine-patches)
- [License](#license)

---

## Overview

Unreal Engine 5.7 ships a DSTM framework (`UE::RemoteObject::Transfer`) that can serialize any actor — including its player controller, possessed pawn, and all subobjects — and reconstitute it on another server without the client noticing a disconnect. By default, the engine expects a disk- or platform-specific transport layer to move the serialized blob between servers. This plugin provides that transport layer on top of the `MultiServerReplication` beacon mesh.

The plugin consists of these cooperating classes:

| Class | Responsibility |
|-------|---------------|
| `FMultiServerReplicationExModule` | Module startup: initializes the server's `FRemoteServerId` and pre-binds the engine transport delegates |
| `UDSTMSubsystem` | Runtime: manages the DSTM beacon mesh, routes outgoing and incoming migration data, handles pull-requests |
| `ADSTMBeaconClient` | Network: extends `AMultiServerBeaconClient` with reliable RPCs that carry serialized `FRemoteObjectData` |
| `UProxyNetDriver` | Proxy: routes clients to multiple game servers (extended with dynamic add/remove/crash detection, HTTP registration) |
| `UProxyRegistrationSubsystem` | `UWorldSubsystem`: auto-detects `-ProxyRegistrationPort=` or `-JoinProxy=` on world begin play and activates the HTTP registration path — no game code needed |

---

## Changes from Stock MultiServerReplication

### Module rename

All modules are renamed to avoid collision with the engine's built-in plugin:

| Stock | Fork |
|---|---|
| `MultiServerReplication` | `MultiServerReplicationEx` |
| `MultiServerConfiguration` | `MultiServerConfigurationEx` |
| `MULTISERVERREPLICATION_API` | `MULTISERVERREPLICATIONEX_API` |
| `MULTISERVERCONFIGURATION_API` | `MULTISERVERCONFIGURATIONEX_API` |
| `/Script/MultiServerReplication.*` | `/Script/MultiServerReplicationEx.*` |

### Proxy Fix 1: Route existing clients to dynamically added servers

```cpp
// UProxyNetDriver (public)
void RegisterGameServerAndConnectClients(const FURL& GameServerURL);
```

Calls `RegisterGameServer()` then iterates all current `ClientConnections`, calling `ConnectToGameServer()` for each open proxy connection so existing players get routes to the new server immediately.

`ConnectToGameServer()` on `UProxyListenerNotify` was also moved from `private` to `public` to enable this.

### Proxy Fix 2: Remove a game server at runtime

```cpp
// UProxyNetDriver (public)
void UnregisterGameServer(int32 GameServerIndex);
```

Full cleanup when removing a game server:
- Closes all proxy routes (`UProxyRoute`) that reference the server's `ParentGameServerConnection`
- Removes players via `RemoveLocalPlayer()`
- Destroys the backend `UIpNetDriver` for that server
- Removes the entry from `GameServerConnections`
- Clamps `PrimaryGameServerForNextClient` to remain valid

### Proxy Fix 3: Detect game server crashes

```cpp
// Delegate (global)
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnGameServerDisconnected, int32 /*GameServerIndex*/, const FURL& /*GameServerURL*/);

// UProxyNetDriver (public)
FOnGameServerDisconnected OnGameServerDisconnected;
```

`DetectGameServerDisconnections()` is called at the start of every `TickFlush()`. It iterates `GameServerConnections` in reverse, and when a connection reaches `USOCK_Closed`:
1. Broadcasts `OnGameServerDisconnected` with the server index and URL
2. Calls `UnregisterGameServer()` for full cleanup

Subscribe to the delegate to react to crashes (e.g., notify an orchestrator, migrate players):

```cpp
ProxyNetDriver->OnGameServerDisconnected.AddLambda(
    [](int32 Index, const FURL& URL)
    {
        UE_LOG(LogTemp, Warning, TEXT("Game server %d (%s) disconnected"), Index, *URL.ToString());
    });
```

### Integrated DSTM transport

The DSTM transport classes (`UDSTMSubsystem`, `ADSTMBeaconClient`) and the module startup code (server identity initialization, transport delegate binding) are built into the `MultiServerReplicationEx` module. No separate plugin is needed.

### Proxy Fix 4: Dynamic game server registration via HTTP

```cpp
// UProxyNetDriver (public)
void StartRegistrationHTTP(int32 Port);
void StopRegistrationHTTP();
static void JoinProxyHTTP(UWorld* World);
```

Game servers can register with the proxy at runtime via HTTP instead of being listed statically in `-ProxyGameServers=`. The proxy starts an HTTP listener (`FHttpServerModule` / `IHttpRouter`) on the port specified by `-ProxyRegistrationPort=`. Game servers send an HTTP POST to the proxy's `/register` endpoint with their listen address, server ID, and DSTM port as query parameters. The proxy registers the game server via `RegisterGameServerAndConnectClients()` and responds with the DSTM peer addresses of all other registered servers so the new server can join the DSTM mesh.

The entire flow is automated by `UProxyRegistrationSubsystem` — a `UWorldSubsystem` that detects the relevant command-line flags and activates the HTTP path on world begin play. No game code is needed. See [Dynamic Proxy Registration (HTTP)](#dynamic-proxy-registration-http).

---

## Prerequisites

- [Unreal Engine 5.7 Source Code ](https://www.unrealengine.com/en-US/ue-on-github)
- `UE_WITH_REMOTE_OBJECT_HANDLE=1` defined in your server target (see [Engine Build Requirement](#engine-build-requirement))
- A dedicated-server topology where each server process has a unique string ID

---

## Adding the Plugin to Your Project

1. Add the plugin to your project's `Plugins/` directory (or as a git submodule).

2. Add `MultiServerReplicationEx` to the plugins list in your `.uproject` file:

   ```json
   {
     "Plugins": [
       { "Name": "MultiServerReplicationEx", "Enabled": true }
     ]
   }
   ```

   Make sure the stock `MultiServerReplication` is **not** also enabled — the two plugins define overlapping classes.

3. Add `MultiServerReplicationEx` to your game module's `PrivateDependencyModuleNames` (only needed if your game code directly references plugin classes like `UDSTMSubsystem`):

   ```cs
   // YourGame.Build.cs
   PrivateDependencyModuleNames.Add("MultiServerReplicationEx");
   ```

   > **Note:** If you only use the plugin via command-line arguments and let `UProxyRegistrationSubsystem` handle registration automatically, you don't need this dependency.

4. Use the `Ex` module name in any `-NetDriverOverrides` or class path references:

   ```
   -NetDriverOverrides=/Script/MultiServerReplicationEx.ProxyNetDriver
   ```

5. Rebuild your project.

---

## Engine Build Requirement

This plugin requires `UE_WITH_REMOTE_OBJECT_HANDLE=1` to be defined at compile time. The stock UE 5.7 default is `0`.

> **Important:** `UE_WITH_REMOTE_OBJECT_HANDLE` is **not supported in editor targets**. Setting it globally in engine headers (e.g. `CoreMiscDefines.h`) will cause compilation failures across the editor codebase (`UE_WITH_OBJECT_HANDLE_LATE_RESOLVE`, `UE_WITH_PACKAGE_ACCESS_TRACKING`, and related subsystems become disabled, breaking cooker, property serialization, and object-ref code). Only enable it when packaging a dedicated client/server pair.

### Recommended approach: Server target `GlobalDefinitions`

Add the define in your **server target** (`.Target.cs`) so it is only active for server builds:

```cs
// YourGameServer.Target.cs
public class YourGameServerTarget : TargetRules
{
    public YourGameServerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Server;
        // ...

        // Enable DSTM remote object handles for cross-server actor migration.
        // Not supported in editor targets — only set for packaged server builds.
        GlobalDefinitions.Add("UE_WITH_REMOTE_OBJECT_HANDLE=1");
    }
}
```

If your project also uses a custom client target that must interoperate with the DSTM server, add the same line to the client `.Target.cs` as well.

You must use an engine built from source (e.g. from the [EpicGames/UnrealEngine](https://github.com/EpicGames/UnrealEngine) repository). A precompiled engine installed via the Epic Games Launcher does not support recompiling engine modules with custom defines. The define propagates through UBT to every module compiled for that target, which requires the engine source to be present.

### Required engine source patches

`GlobalDefinitions` applies `UE_WITH_REMOTE_OBJECT_HANDLE=1` to **every module** compiled for the target, including engine modules. Several engine source files contain hardcoded size assumptions about `FWeakObjectPtr` that break when the define changes its layout from 8 to 16 bytes. These must be patched in the engine source before building:

**`Engine/Source/Runtime/MovieScene/Public/TrackInstancePropertyBindings.h`** — `FVolatilePropertyStep` size assertion:

```cpp
// Replace the static_assert around line 92:
#if UE_WITH_REMOTE_OBJECT_HANDLE
static_assert(sizeof(FVolatilePropertyStep) <= 24, "Try to fit FVolatilePropertyStep inside 24 bytes");
#else
static_assert(sizeof(FVolatilePropertyStep) <= 16, "Try to fit FVolatilePropertyStep inside 16 bytes");
#endif
```

**`Engine/Source/Runtime/Engine/Private/Collision/SceneQuery.cpp`** — unreachable code in `GetIgnoreQueryHandler()`:

```cpp
// Replace the function around line 41:
bool GetIgnoreQueryHandler()
{
#if UE_WITH_REMOTE_OBJECT_HANDLE
	return bIgnoreQueryHandler;
#else
	return false;
#endif
}
```

The original code has `return false;` after the `#if` block's `return` without an `#else`, which MSVC treats as unreachable code (error C4702 with warnings-as-errors).

> **Note:** Additional patches may be needed if your project enables optional engine plugins (e.g. `PlainPropsUObject`) that also contain `FWeakObjectPtr` size assertions. If you encounter similar `static_assert` failures during the build, apply the same conditional pattern.

If the define is `0` (or absent), the plugin compiles but the DSTM transport remains inert: the module logs a warning, skips delegate binding, and the subsystem reports `IsMeshActive() == false`. The proxy fixes still work regardless. No engine patches are needed in that case.

> **ABI note:** Setting `UE_WITH_REMOTE_OBJECT_HANDLE=1` changes `FWeakObjectPtr` layout (adds `FRemoteObjectId`, growing it from 8 to 16 bytes). All modules linked into the server binary must be compiled with the same setting. This happens automatically when using `GlobalDefinitions` in the target — UBT recompiles everything for that target configuration.

### Runtime requirement: Disable garbage elimination

At runtime, `UE_WITH_REMOTE_OBJECT_HANDLE=1` requires garbage elimination to be disabled. If it is not, the server crashes on startup with:

```
Assertion failed: !IsGarbageEliminationEnabled()
"Remote object support requires garbage elimination to be disabled"
[ObjectBaseUtility.cpp]
```

Pass `-DisableGarbageElimination` on the command line to every server process that was built with `UE_WITH_REMOTE_OBJECT_HANDLE=1`. This sets the CVar `gc.GarbageEliminationEnabled` to `false` during `InitGarbageElimination()`. The flag is parsed in `Engine/Source/Runtime/CoreUObject/Private/UObject/ObjectBaseUtility.cpp`.

> **Note:** The proxy server also requires this flag if it was compiled from the same source-built engine with `UE_WITH_REMOTE_OBJECT_HANDLE=1`.

### NetDriver configuration in DefaultEngine.ini

The engine multi-server mesh requires a `NetDriverDefinitions` entry in `DefaultEngine.ini`. Use the `Ex` module name to match this plugin:

```ini
[/Script/Engine.Engine]
+NetDriverDefinitions=(DefName="MultiServerNetDriver",DriverClassName="/Script/MultiServerReplicationEx.MultiServerNetDriver",DriverClassNameFallback="/Script/MultiServerReplicationEx.MultiServerNetDriver")
```

> **Common mistake:** If using the stock module path `/Script/MultiServerReplication.MultiServerNetDriver`, the driver will fail to load: `CreateNamedNetDriver failed to create driver from definition MultiServerNetDriver`. Always use `/Script/MultiServerReplicationEx.*` with this plugin.

### Network version compatibility (mixed builds)

If you build your dedicated server from engine source but use the installed (Epic Launcher) engine for editor/client builds, the server's `FNetworkVersion` changelist will be `0` while the client's will be a non-zero value (e.g., `47537391`). This causes the client to be rejected with `NMT_Failure` / `OutdatedClient`.

To allow mixed-build connections, override the network version check in your game module:

```cpp
#include "Misc/NetworkVersion.h"

void FYourGameModule::StartupModule()
{
    FNetworkVersion::IsNetworkCompatibleOverride.BindLambda(
        [](uint32 LocalNetworkVersion, uint32 RemoteNetworkVersion)
        {
            return true; // Allow source-built server ↔ installed client
        });
}
```

> **Warning:** This bypasses all network version checking. Do not ship this in production — use a more targeted check that validates your own protocol version instead.

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
  (bound by module StartupModule)
            │
            ▼
  HandleOutgoingMigration()
  [FMemoryWriter → TArray<u8>]
            │
            └────── beacon RPC ──────►
                                      ServerReceive/           Beacon receives
                                      ClientReceive            migration data
                                      MigratedObject()               │
                                                                     ▼
                                                         HandleIncomingMigrationData()
                                                         [FMemoryReader → TArray<u8>]
                                                                     │
                                                                     ▼
                                                         OnObjectDataReceived()
                                                         [engine DSTM receive pipeline]
                                                                     │
                                                                     ▼
                                                         AActor::PostMigrate(Receive)
                                                         APlayerController::PostMigrate(Receive)
```

The DSTM beacon mesh is a separate `UMultiServerNode` instance from any game-level multi-server mesh, keeping the transport concern isolated.

---

## Command-Line Arguments

This plugin inherits the stock `MultiServerReplication` command-line arguments and adds DSTM transport arguments. All arguments are listed below grouped by component.

### DSTM transport arguments

Each server process that participates in the DSTM mesh must receive these arguments:

| Argument | Required | Description |
|----------|----------|-------------|
| `-DedicatedServerId=<string>` | Yes | Unique string identifier for this server (e.g. `server-1`). Hashed into a 10-bit `FRemoteServerId` in range [1, 1020] via `GetTypeHash() % 1020 + 1`. Also used as the DSTM beacon mesh `LocalPeerId` for peer identification. **Proxy servers also need this flag** — `FRemoteServerId::InitGlobalServerId()` must be called before the engine touches any remote object types, even on the proxy. |
| `-DisableGarbageElimination` | Yes | Disables UE garbage elimination (CVar `gc.GarbageEliminationEnabled`). **Required at runtime** when the binary is compiled with `UE_WITH_REMOTE_OBJECT_HANDLE=1` — the server will crash on startup without it. See [Engine Build Requirement](#engine-build-requirement). |
| `-DSTMListenPort=<int>` | Yes | Port for the DSTM beacon listener. Each server needs a unique port (per host). Defaults to `16000`. |
| `-DSTMListenIp=<ip>` | No | IP address to bind the DSTM beacon listener. Useful when servers should communicate over a private network interface separate from the game port. Defaults to `0.0.0.0`. |
| `-DSTMPeers=<ip:port,...>` | Yes (multi-server) | Comma-separated list of `host:port` pairs pointing to other servers' DSTM beacon ports. |

The expected server count for `AreAllPeersConnected()` is derived automatically as `PeerAddresses.Num() + 1` (peers + self). No separate count argument is needed.

> **Note: No GUID seed is needed.** With `UE_WITH_REMOTE_OBJECT_HANDLE=1`, every `FNetworkGUID` is derived from `FRemoteObjectId`, which embeds the 10-bit `ServerId` — collisions between servers are structurally impossible. The seed-based `FNetGUIDCache` counter (`NetworkGuidIndex`) is compile-time excluded by the DSTM code path.

### Engine multi-server mesh arguments (stock)

These arguments are parsed by `UMultiServerNode::ParseCommandLineIntoCreateParams()` from the stock `MultiServerReplication` plugin. They configure the engine-level beacon mesh used for server-to-server replication metadata. The DSTM mesh is **separate** from this mesh and uses its own args above.

| Argument | Required | Description |
|----------|----------|-------------|
| `-MultiServerLocalId=<string>` | Yes | Unique peer ID for this server in the engine mesh. Typically set to the same value as `-DedicatedServerId=`. |
| `-MultiServerListenPort=<int>` | Yes | Port for the engine beacon mesh listener. Each server needs a unique port. |
| `-MultiServerPeers=<ip:port,...>` | Yes (multi-server) | Comma-separated list of `host:port` pairs pointing to other servers' engine mesh beacon ports. |
| `-MultiServerNumServers=<int>` | No | Expected number of servers for `AreAllServersConnected()`. Defaults to the peer count if omitted. Rarely needed — the peer list length is usually sufficient. |

### Proxy arguments (stock + extended)

These arguments configure the proxy server that multiplexes player connections to backend game servers:

| Argument | Required | Description |
|----------|----------|-------------|
| `-ProxyGameServers=<addresses>` | No (proxy only) | Comma-separated list of backend game server addresses for **static** registration at startup. Supports port ranges (`127.0.0.1:7777-7778` expands to two entries). Not needed when using dynamic beacon registration. |
| `-ProxyRegistrationPort=<int>` | No (proxy only) | Port for the **dynamic** HTTP registration listener. Game servers POST to this endpoint at runtime and register themselves automatically. See [Dynamic Proxy Registration (HTTP)](#dynamic-proxy-registration-http). |
| `-JoinProxy=<host:port>` | No (game server only) | Address of the proxy's HTTP registration endpoint. The game server sends an HTTP POST to register itself and receives DSTM peer addresses in the response. Parsed automatically by `UProxyRegistrationSubsystem`. |
| `-NetDriverOverrides=<class>` | Yes (proxy only) | Must be set to `/Script/MultiServerReplicationEx.ProxyNetDriver` to activate the proxy. |
| `ProxyClientPrimaryGameServer=<int\|random>` | No | Which game server index is the primary for each new client. Pass `random` for randomization. Defaults to `0`. |
| `-ProxyCyclePrimaryGameServer` | No | Flag: after each client connects, advance `PrimaryGameServerForNextClient` to the next server (round-robin). |

> **Note:** `-ProxyGameServers=` and `-ProxyRegistrationPort=` can be used together. Static servers are registered immediately at startup; additional servers can join later via HTTP. If neither is provided, the proxy starts with zero game servers and waits for HTTP registrations.

### Example (two-server cluster with proxy)

A complete local launch requires three kinds of arguments per game server:

1. **Engine / game** — `-server -port=<game-port>`
2. **Engine multi-server mesh** — `-MultiServerListenPort=<port> -MultiServerPeers=<peer-mesh-addresses>`
3. **DSTM transport** — `-DedicatedServerId=<id> -DSTMListenPort=<port> -DSTMPeers=<peer-dstm-addresses>`

When using dynamic HTTP registration (`-JoinProxy=`), items 2 and 3 are simplified because the DSTM peer list is discovered automatically from the proxy.

#### Direct mode (no proxy, or proxy with static `-ProxyGameServers=`)

```
# Server 1 (game 7777, engine mesh 15000, DSTM beacon 16000)
-server -port=7777 -DisableGarbageElimination
-MultiServerListenPort=15000 -MultiServerPeers=127.0.0.1:15001
-DedicatedServerId=server-1 -DSTMListenPort=16000 -DSTMPeers=127.0.0.1:16001

# Server 2 (game 7778, engine mesh 15001, DSTM beacon 16001)
-server -port=7778 -DisableGarbageElimination
-MultiServerListenPort=15001 -MultiServerPeers=127.0.0.1:15000
-DedicatedServerId=server-2 -DSTMListenPort=16001 -DSTMPeers=127.0.0.1:16000

# Proxy — OPTION A: static registration (servers known at launch)
-server -port=7780 -DisableGarbageElimination
-DedicatedServerId=proxy-1
-NetDriverOverrides=/Script/MultiServerReplicationEx.ProxyNetDriver
-ProxyGameServers=127.0.0.1:7777,127.0.0.1:7778

# Proxy — OPTION B: dynamic registration (servers register via HTTP)
-server -port=7780 -DisableGarbageElimination
-DedicatedServerId=proxy-1
-NetDriverOverrides=/Script/MultiServerReplicationEx.ProxyNetDriver
-ProxyRegistrationPort=17000
```

#### Dynamic HTTP mode (game servers discover DSTM peers via proxy)

When using `-JoinProxy=`, game servers don't need `-DSTMPeers=` — the proxy returns peer addresses in the HTTP response:

```
# Server 1 (game 7777, DSTM beacon 16000, registers with proxy via HTTP)
-server -port=7777 -DisableGarbageElimination
-DedicatedServerId=server-1 -DSTMListenPort=16000
-JoinProxy=127.0.0.1:17000

# Server 2 (game 7778, DSTM beacon 16001, registers with proxy via HTTP)
-server -port=7778 -DisableGarbageElimination
-DedicatedServerId=server-2 -DSTMListenPort=16001
-JoinProxy=127.0.0.1:17000

# Proxy (HTTP registration listener on 17000)
-server -port=7780 -DisableGarbageElimination
-DedicatedServerId=proxy-1
-NetDriverOverrides=/Script/MultiServerReplicationEx.ProxyNetDriver
-ProxyRegistrationPort=17000
```

| Port | Purpose |
|---|---|
| 7777–7778 | Game ports (client/proxy ↔ game server, UDP) |
| 15000–15001 | Engine multi-server mesh (server ↔ server beacons for replication) |
| 16000–16001 | DSTM beacon mesh (server ↔ server beacons for migration transport) |
| 7780 | Proxy listener (client ↔ proxy, UDP) |
| 17000 | Proxy HTTP registration (game server → proxy, TCP/HTTP) — only when using `-ProxyRegistrationPort=` |

The proxy does **not** need DSTM mesh arguments (`-DSTMListenPort`, `-DSTMPeers`) — it forwards client traffic to game servers but does not participate in server-to-server migration. However, it **does** need `-DedicatedServerId=` and `-DisableGarbageElimination` because the global server identity must be initialized before the engine touches any remote object types.

When using dynamic HTTP registration (`-ProxyRegistrationPort=` / `-JoinProxy=`), game servers do **not** need `-DSTMPeers=` — the DSTM peer list is discovered automatically from the proxy's HTTP response.

---

## Initialization

### Why `Initialize()` does not auto-start the mesh

`UGameInstanceSubsystem::Initialize()` runs during `GameInstance` creation, before any `UWorld` exists. `GetWorld()` returns `nullptr` at that point. Creating a beacon mesh requires a valid world, so the subsystem starts inert and waits to be told when to initialize.

### Calling from your Game Mode

> **Note:** Proxy registration is handled automatically by `UProxyRegistrationSubsystem` and does **not** require any game code. You only need to call `InitializeFromCommandLine()` for the DSTM subsystem — and only when the server is **not** using `-JoinProxy=` (because `JoinProxyHTTP()` initializes the DSTM mesh automatically from the proxy's response).

Call `InitializeFromCommandLine()` from your game mode's `StartPlay()` (or equivalent) once the world is ready:

```cpp
// YourGameMode.cpp
#include "DSTMSubsystem.h"

void AYourGameMode::StartPlay()
{
    Super::StartPlay();

    // Proxy registration is handled automatically by UProxyRegistrationSubsystem.
    // DSTM init is also automatic when using -JoinProxy= (peers come from the
    // proxy HTTP response). Only call InitializeFromCommandLine() for direct
    // peer-to-peer mode (using -DSTMPeers= without a proxy).

    const bool bUsingProxy = FString(FCommandLine::Get()).Contains(TEXT("-JoinProxy="));

    if (!bUsingProxy)
    {
        if (UGameInstance* GI = GetGameInstance())
        {
            if (UDSTMSubsystem* DSTM = GI->GetSubsystem<UDSTMSubsystem>())
            {
                DSTM->InitializeFromCommandLine();
            }
        }
    }
}
```

`InitializeFromCommandLine()` reads the command-line arguments described above and calls `InitializeDSTMMesh()` internally. It returns `true` if the mesh was created or `false` if the process is not in multi-server mode (no `-DedicatedServerId=` present).

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

> **Important:** The possessed `Pawn` is a **separate actor**, not a subobject of the PlayerController. `TransferObjectOwnershipToRemoteServer(PC)` transfers the PC and its component hierarchy, but **does not include the Pawn**. Your game code must handle pawn lifecycle on both sides — see [Pawn Handling During Migration](#pawn-handling-during-migration).

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

## Pawn Handling During Migration

The DSTM transfer serializes the PlayerController and its **component subobjects** (e.g., `USceneComponent` root, camera manager). The possessed **Pawn** (e.g., `ACharacter`) is a separate actor and is **not** included in the transfer. Your game code is responsible for:

1. **Sending server** — destroy the pawn before (or immediately after) calling `TransferActorToServer()`
2. **Receiving server** — detect the pawnless migrated PC and spawn a new pawn

### Sending server: stamp position and destroy pawn

Before transferring the PC, capture the pawn's world transform and stamp it onto the PC's root component so the receiving server knows where to place the new pawn. Then destroy the pawn to prevent a "ghost" (frozen character with last animation) remaining visible on the sending server.

```cpp
void AYourGameMode::MigratePlayer(APlayerController* PC, ACharacter* Pawn)
{
    UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();
    FRemoteServerId DestId;
    if (!DSTM || !DSTM->GetFirstPeerServerId(DestId)) return;

    // 1. Capture pawn position
    const FVector PawnPos = Pawn->GetActorLocation();
    const FRotator PawnRot = Pawn->GetActorRotation();

    // 2. Stamp onto PC's root component (AController hides SetActorLocation)
    if (USceneComponent* Root = PC->GetRootComponent())
    {
        Root->SetWorldLocationAndRotation(PawnPos, PawnRot);
    }

    // 3. Destroy pawn on sending server
    PC->UnPossess();
    Pawn->Destroy();

    // 4. Transfer the pawnless PC — position is carried in root component
    DSTM->TransferActorToServer(PC, DestId);
}
```

> **Why destroy the pawn?** If the pawn survives on the sending server, `CheckZoneBoundaries()` (or similar logic) will read the pawn's drifting position and trigger another migration on the next tick — causing an infinite ping-pong loop between servers. _The pawn must not exist on the sending server after migration._

### Receiving server: detect migrated PC and spawn pawn

After `PostMigrate(Receive)` binds the PC to the `ChildConnection`, the PC appears in the player controller iterator with a valid `Player` but no possessed pawn. Detect this in your tick/boundary-check and spawn a fresh pawn:

```cpp
void AYourGameMode::HandleMigratedPlayerArrival(APlayerController* PC)
{
    // Read position from the PC's root component (stamped by sending server)
    USceneComponent* Root = PC->GetRootComponent();
    const FVector Pos = Root ? Root->GetComponentLocation() : FVector::ZeroVector;
    const FRotator Rot = Root ? Root->GetComponentRotation() : FRotator::ZeroRotator;

    // Spawn pawn at the exact migrated position
    const FTransform SpawnTransform(Rot, Pos);
    APawn* NewPawn = SpawnDefaultPawnAtTransform(PC, SpawnTransform);
    if (NewPawn)
    {
        PC->Possess(NewPawn);
    }
}
```

### Avoiding ping-pong

When the pawn spawns on the receiving server, it appears right at (or very close to) the zone boundary. Without a grace period, the next boundary check could immediately migrate the player back. Use a transfer arrival timestamp to suppress migration for a short window:

```cpp
// On arrival:
TransferArrivalTimes.Add(PC, GetWorld()->GetTimeSeconds());

// In boundary check:
if (const double* ArrivalTime = TransferArrivalTimes.Find(PC))
{
    if (CurrentTime - *ArrivalTime < GracePeriodSeconds)
        continue; // Skip this PC during grace period
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
  └─ FMultiServerReplicationExModule::OnRemoteObjectTransfer()
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

A "pull" migration happens when a destination server requests an object that still lives on another server — for example, when the engine's DSTM scheduler determines that an object should move before the source server has initiated it.

The engine calls `RequestRemoteObjectDelegate` on the destination server. The plugin handles this with `HandleObjectRequest()`:

1. Looks up the beacon for `LastKnownServerId`
2. Sends `ServerRequestMigrateObject()` or `ClientRequestMigrateObject()` depending on beacon authority

The source server receives the request, fires `OnMigrationRequested`, and `HandleIncomingMigrationRequest()`:

1. Iterates all world actors, matching `FRemoteObjectHandle.GetRemoteObjectId()` against the requested `FRemoteObjectId`
2. Calls `TransferActorToServer(FoundActor, RequestingServerId)` — which triggers the normal push flow

---

## Beacon Mesh

The plugin creates its own `UMultiServerNode` for the DSTM transport. This is separate from any game-level multi-server mesh, keeping the transport concern fully isolated.

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
-server -port=7779
-MultiServerListenPort=15002 -MultiServerPeers=192.168.1.10:15000,192.168.1.11:15001
-DedicatedServerId=server-3 -DSTMListenPort=16000 -DSTMPeers=192.168.1.10:16000,192.168.1.11:16000
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

---

## Dynamic Proxy

The MultiServer Proxy (`UProxyNetDriver`) is **fully dynamic** in this fork. Its game server list is typically parsed from `-ProxyGameServers=` during `InitBase()`, but servers can be added and removed at runtime.

### What works

- `RegisterGameServer(const FURL&)` can be called post-init — it appends to the `GameServerConnections` array
- `RegisterGameServerAndConnectClients(const FURL&)` does the same plus routes all existing proxy clients to the new server
- `UnregisterGameServer(int32)` removes a game server at runtime with full route/player/driver cleanup
- `OnGameServerDisconnected` delegate fires when a game server crashes or disconnects — `DetectGameServerDisconnections()` runs every `TickFlush()`
- Clients that connect **after** a dynamic registration automatically get routes to the new server (the proxy iterates the full `GameServerConnections` array on each `NMT_Join`)
- `UProxyNetDriver` is exported (`MULTISERVERREPLICATIONEX_API`) and can be subclassed by plugins
- `UProxyRegistrationSubsystem` auto-activates HTTP registration from command-line flags — no game code needed
- HTTP-registered game servers automatically receive DSTM peer addresses in the response, enabling zero-config DSTM mesh setup

### Orchestration notes

The automation of scaling decisions is **out of scope** for this plugin. An external application should handle:
- When to spin up / tear down server instances
- Assigning unique `-DedicatedServerId=` values
- Providing the correct `-DSTMPeers=` addresses to new servers
- Deciding which server to migrate players *to* before shutting down a server

---

## Dynamic Proxy Registration (HTTP)

Instead of listing game servers statically with `-ProxyGameServers=`, the proxy can accept dynamic registrations over HTTP. Game servers send an HTTP POST to the proxy with their listen address, DSTM port, and server ID. The proxy registers the game server via `RegisterGameServerAndConnectClients()` — the same code path as static registration — so existing clients get routes to the new server immediately. The proxy also returns the DSTM peer addresses of all other registered servers, enabling the new game server to join the DSTM mesh automatically.

### Automatic activation via `UProxyRegistrationSubsystem`

All proxy registration logic is handled automatically by `UProxyRegistrationSubsystem`, a `UWorldSubsystem` built into the plugin. It detects the relevant command-line flags and activates the correct HTTP path on world begin play:

- **Proxy process** with `-ProxyRegistrationPort=<port>`: calls `UProxyNetDriver::StartRegistrationHTTP()` to start the HTTP listener.
- **Game server process** with `-JoinProxy=<host:port>`: calls `UProxyNetDriver::JoinProxyHTTP()` to send an HTTP POST to the proxy.

**No game code is required.** The subsystem is only created on dedicated servers that have one of these flags.

### Proxy side

Pass `-ProxyRegistrationPort=<port>` on the proxy's command line. The subsystem calls `StartRegistrationHTTP()`, which binds a POST handler on `/register` using `FHttpServerModule` / `IHttpRouter`.

When a game server POSTs to `/register?address=<host:port>&serverId=<id>&dstmPort=<port>`, the handler:
1. Extracts the `address`, `serverId`, and `dstmPort` query parameters
2. Calls `RegisterGameServerAndConnectClients()` to add the game server to the proxy
3. Stores the game server's DSTM address (`host:dstmPort`) in `RegisteredDSTMPeers`
4. Returns a single-line response containing the comma-separated DSTM peer addresses of all *other* registered servers

The HTTP listener is automatically stopped during `UProxyNetDriver::Shutdown()`.

### Game server side

Pass `-JoinProxy=<host:port>` on the game server's command line (e.g., `-JoinProxy=127.0.0.1:17000`). The subsystem calls `JoinProxyHTTP()`, which:
1. Reads `-JoinProxy=`, `-DedicatedServerId=`, and `-DSTMListenPort=` from the command line
2. Computes the game server's listen address from the world URL port
3. Sends an async HTTP POST to `http://<proxy>/register?address=<addr>&serverId=<id>&dstmPort=<port>`
4. On response, parses the comma-separated DSTM peer addresses
5. Initializes the DSTM mesh via `UDSTMSubsystem::InitializeDSTMMesh()` with the discovered peers

### How it works

1. Proxy starts with `-ProxyRegistrationPort=17000` → `UProxyRegistrationSubsystem` calls `StartRegistrationHTTP(17000)`
2. Game server starts with `-JoinProxy=127.0.0.1:17000` → `UProxyRegistrationSubsystem` calls `JoinProxyHTTP()`
3. Game server POSTs to `http://127.0.0.1:17000/register?address=127.0.0.1:7777&serverId=server-1&dstmPort=16000`
4. Proxy receives the POST, registers the game server, returns DSTM peers of other already-registered servers
5. Game server parses the DSTM peer list and initializes its DSTM beacon mesh
6. All existing proxy clients get routes to the new server; future clients get them on `NMT_Join`

### Combining static and dynamic registration

You can use both modes together:
- `-ProxyGameServers=127.0.0.1:7777` — server-1 is registered at startup
- `-ProxyRegistrationPort=17000` — server-2, server-3, etc. register later via HTTP

This is useful when some servers are always present (static) and others scale dynamically.

### Why HTTP instead of beacons?

The proxy world already contains a `UIpNetDriver` for client traffic. Adding a second `UIpNetDriver` for a beacon-based registration handshake causes UDP `StatelessConnectHandlerComponent` collisions — both drivers try to process the same inbound packets. HTTP avoids this entirely by using a separate TCP-based transport that does not conflict with the game's UDP net drivers.

---

## Tip: Seamless Transfer with a Custom CMC

DSTM migration transfers the PlayerController and spawns a new pawn on the destination server. During the transit gap (the time between the source server serializing the actor and the destination server processing the first client moves), the client keeps predicting movement locally. This causes two problems on the destination server:

1. **Height displacement** — If the pawn returns to a server it previously lived on, the engine reuses the zombie actor whose `CharacterMovementComponent` still holds stale `ServerPredictionData`. The stale timestamps cause `ForcePositionUpdate` to compute a huge `DeltaTime` (`world_time - old_timestamp`), triggering max-iteration warnings and displacing the character vertically.

2. **Position jitter** — The first client moves to arrive carry positions that have drifted from the serialized spawn point by `speed × transit_time`. The default position error threshold (~1.73 UU from `MAXPOSITIONERRORSQUARED = 3.0`) rejects these as errors, triggering a server correction that snaps the client back — visible as a jitter/rubber-band.

Both problems can be solved with a custom `UCharacterMovementComponent` subclass that resets stale state on arrival and temporarily widens the position error tolerance.

### Example: Custom CMC for seamless DSTM transfer

```cpp
// YourCharacterMovementComponent.h

UCLASS()
class UYourCharacterMovementComponent : public UCharacterMovementComponent
{
    GENERATED_BODY()

public:
    /**
     * Call this from your GameMode's migrated-player-arrival handler,
     * after the pawn is possessed but before the first tick processes client moves.
     */
    void ResetForDSTMArrival();

protected:
    virtual bool ServerExceedsAllowablePositionError(
        float ClientTimeStamp, float DeltaTime, const FVector& Accel,
        const FVector& ClientWorldLocation, const FVector& RelativeClientLocation,
        UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName,
        uint8 ClientMovementMode) override;

private:
    /** World time when the most recent DSTM arrival was processed. 0 = no active grace. */
    float DSTMArrivalWorldTime = 0.f;

    /** Speed at the moment of DSTM arrival (for tolerance calculation). */
    float DSTMArrivalSpeed = 0.f;

    /** How long (seconds) the grace window lasts after DSTM arrival. */
    static constexpr float DSTMGraceDuration = 5.0f;

    /** Max expected DSTM transit time (seconds). Tolerance decays from this to zero. */
    static constexpr float DSTMTransitAllowance = 0.5f;

    /** Fixed safety margin (UU) always added to the speed-based tolerance. */
    static constexpr float DSTMSafetyMargin = 50.f;
};
```

```cpp
// YourCharacterMovementComponent.cpp

void UYourCharacterMovementComponent::ResetForDSTMArrival()
{
    // Fix 1: Reset stale ServerPredictionData from the zombie pawn's previous
    // lifecycle on this server. Without this, ForcePositionUpdate computes a
    // huge DeltaTime and displaces the character vertically.
    if (HasPredictionData_Server())
    {
        FNetworkPredictionData_Server_Character* ServerData = GetPredictionData_Server_Character();
        ServerData->ServerTimeStamp = 0.f;
        ServerData->CurrentClientTimeStamp = 0.f;
        ServerData->ServerAccumulatedClientTimeStamp = 0.0;
        ServerData->ResetForcedUpdateState();
    }

    // Fix 2: Start a position-tolerance grace window. The tolerance starts
    // high to absorb the transit drift and decays back to just the margin.
    if (const UWorld* World = GetWorld())
    {
        DSTMArrivalWorldTime = World->GetTimeSeconds();
        DSTMArrivalSpeed = Velocity.Size();
    }
}

bool UYourCharacterMovementComponent::ServerExceedsAllowablePositionError(
    float ClientTimeStamp, float DeltaTime, const FVector& Accel,
    const FVector& ClientWorldLocation, const FVector& RelativeClientLocation,
    UPrimitiveComponent* ClientMovementBase, FName ClientBaseBoneName,
    uint8 ClientMovementMode)
{
    if (DSTMArrivalWorldTime > 0.f)
    {
        const UWorld* World = GetWorld();
        const float TimeSinceArrival = World
            ? (World->GetTimeSeconds() - DSTMArrivalWorldTime)
            : DSTMGraceDuration;

        if (TimeSinceArrival < DSTMGraceDuration)
        {
            const FVector ServerLoc = UpdatedComponent->GetComponentLocation();
            const float ErrorSq = (ServerLoc - ClientWorldLocation).SizeSquared();

            // Tolerance decays linearly: high at arrival, down to just the
            // safety margin after TransitAllowance seconds.
            const float TransitRemaining = FMath::Max(DSTMTransitAllowance - TimeSinceArrival, 0.f);
            const float Tolerance = DSTMArrivalSpeed * TransitRemaining + DSTMSafetyMargin;

            if (ErrorSq <= FMath::Square(Tolerance))
            {
                // Trust the client — snap server pawn to client position.
                UpdatedComponent->MoveComponent(
                    ClientWorldLocation - ServerLoc,
                    UpdatedComponent->GetComponentQuat(),
                    true, nullptr, EMoveComponentFlags::MOVECOMP_NoFlags,
                    ETeleportType::TeleportPhysics);
                bJustTeleported = true;
                return false; // no correction
            }

            // Beyond tolerance — end grace, fall through to default correction.
            DSTMArrivalWorldTime = 0.f;
        }
        else
        {
            DSTMArrivalWorldTime = 0.f; // grace expired
        }
    }

    return Super::ServerExceedsAllowablePositionError(
        ClientTimeStamp, DeltaTime, Accel, ClientWorldLocation,
        RelativeClientLocation, ClientMovementBase, ClientBaseBoneName,
        ClientMovementMode);
}
```

### Calling `ResetForDSTMArrival()`

In your game mode's migrated-player-arrival handler, after possessing the new pawn:

```cpp
void AYourGameMode::HandleMigratedPlayerArrival(APlayerController* PC)
{
    // ... spawn pawn, possess, etc.

    if (APawn* NewPawn = PC->GetPawn())
    {
        if (auto* CMC = Cast<UYourCharacterMovementComponent>(
                NewPawn->FindComponentByClass<UCharacterMovementComponent>()))
        {
            CMC->ResetForDSTMArrival();
        }
    }
}
```

### Tuning the constants

| Constant | Default | Purpose |
|----------|---------|---------|
| `DSTMGraceDuration` | `5.0` s | Total duration of the grace window. During this time, server snaps to client position when within tolerance. |
| `DSTMTransitAllowance` | `0.5` s | Max expected transit time. Tolerance decays from `speed × 0.5 + margin` down to just `margin` over this period. Increase if your migration takes longer (high latency, large payloads). |
| `DSTMSafetyMargin` | `50.0` UU | Fixed tolerance added on top of the speed-based component. Covers any residual position error after the transit drift has been absorbed. |

At `MaxWalkSpeed = 600` UU/s with the defaults above:
- **At t=0** (first frame after arrival): tolerance = `600 × 0.5 + 50 = 350` UU
- **At t=0.5s**: tolerance = `600 × 0 + 50 = 50` UU
- **At t=0.5–5.0s**: tolerance stays at `50` UU (server still snaps to client)
- **After 5.0s**: normal threshold resumes (~1.73 UU)

---

## Logging

| Log category | Used in |
|---|---|
| `LogDSTM` | `FMultiServerReplicationExModule` — module startup, delegate binding, server identity |
| `LogDSTMSub` | `UDSTMSubsystem` — mesh lifecycle, peer connections, migration send/receive |
| `LogDSTMBeacon` | `ADSTMBeaconClient` — beacon RPC send/receive |
| `LogNetProxy` | `UProxyNetDriver` — proxy lifecycle, game server registration, route management, HTTP registration |
| `LogProxyRegistration` | `UProxyRegistrationSubsystem` — subsystem lifecycle, auto-detection of command-line flags |

Enable verbose output:

```ini
; DefaultEngine.ini
[Core.Log]
LogDSTM=Verbose
LogDSTMSub=Verbose
LogDSTMBeacon=Verbose
LogNetProxy=Verbose
LogProxyRegistration=Verbose
```

---

## Troubleshooting

### `UE_WITH_REMOTE_OBJECT_HANDLE is disabled` warning at startup

Your server build does not have DSTM support compiled in. Add `GlobalDefinitions.Add("UE_WITH_REMOTE_OBJECT_HANDLE=1");` to your server `.Target.cs` file and rebuild. See [Engine Build Requirement](#engine-build-requirement). Do **not** set this define in engine headers — it is not supported in editor targets.

### `No -DedicatedServerId= on command line` — migration never starts

Each server process must receive `-DedicatedServerId=<unique-string>`. Without it, `FRemoteServerId::InitGlobalServerId()` is never called, delegate binding is skipped, and the engine has no server identity for routing.

### `No beacon connection to destination server N! Migration data lost`

The DSTM beacon mesh has not finished connecting to the target server. Ensure:
- Both servers are running and have received `-DSTMPeers=` pointing to each other's DSTM ports
- `AreAllPeersConnected()` returns `true` before initiating the first migration
- Firewall rules allow traffic on the DSTM beacon port

### Ghost pawn / infinite ping-pong after migration

The Pawn is **not** a subobject of the PlayerController — it is a separate actor. `TransferActorToServer(PC)` does not include it. You must:
1. Destroy the pawn on the sending server before or after `TransferActorToServer()`
2. Spawn a new pawn on the receiving server

See [Pawn Handling During Migration](#pawn-handling-during-migration).

If the pawn is not destroyed on the source server, it remains visible as a frozen "ghost" with its last animation pose. Worse, if your boundary-check logic reads the ghost pawn's position, it will trigger an infinite migration loop (ping-pong) because the ghost drifts due to simulated movement.

### `Failed to deserialize FRemoteObjectData` on receive

A serialization version mismatch between the two servers. Both server binaries must be built from the same source. This error can also occur if the network payload was truncated — ensure the beacon allows unlimited bunch sizes (`SetUnlimitedBunchSizeAllowed(true)` is inherited from `AMultiServerBeaconClient`).

### DSTM mesh created but `AreAllPeersConnected()` never returns `true`

`AreAllPeersConnected()` is a startup readiness check: it waits until all peers listed in `-DSTMPeers=` have connected and exchanged IDs. If the peer list is correct but the check never passes, verify network connectivity and that each peer's beacon listener port is reachable.

**In dynamic meshes** where servers join and leave, `AreAllPeersConnected()` becomes unreliable after a server departure — `NumExpectedServers` never decreases, so the check stays `false` permanently. Use `GetConnectedPeerCount() > 0` or `GetConnectedPeerIds()` instead. See [Runtime Scaling](#runtime-scaling).

### `Reassigning NetGUID` warnings / `ObjectReplicatorReceivedBunchFail` crashes

If you see these errors despite using DSTM with `UE_WITH_REMOTE_OBJECT_HANDLE=1`, check that every server has a unique `-DedicatedServerId=`. Duplicate server IDs produce duplicate `FRemoteServerId` values, which can cause identical `FRemoteObjectId` and thus identical `FNetworkGUID` values. Also verify that no hash collision exists (check the log for `DSTM HASH COLLISION` messages).

### `Remote object support requires garbage elimination to be disabled` crash at startup

The server process was compiled with `UE_WITH_REMOTE_OBJECT_HANDLE=1` but launched without `-DisableGarbageElimination`. The assertion fires in `InitGarbageElimination()` before the engine reaches `Init()`. Add `-DisableGarbageElimination` to the server's command line.

### `GlobalServerId.Id != Local...Global server id hasn't been initialized yet` crash (proxy)

The proxy was launched without `-DedicatedServerId=`. Even though the proxy does not participate in migration, the engine's remote object type system requires a global server identity. Add `-DedicatedServerId=proxy-1` (or any unique name) to the proxy command line.

### `CreateNamedNetDriver failed to create driver from definition MultiServerNetDriver`

The `DefaultEngine.ini` `NetDriverDefinitions` entry references an incorrect class path. With this plugin, use:

```ini
+NetDriverDefinitions=(DefName="MultiServerNetDriver",DriverClassName="/Script/MultiServerReplicationEx.MultiServerNetDriver",...)
```

Do **not** use `/Script/MultiServerReplication.MultiServerNetDriver` — that references the stock engine plugin which is not loaded.

### `NMT_Failure` / `OutdatedClient` — server rejects client connection

This happens when the server and client are built with different engines. A source-built server has `FNetworkVersion` changelist `0`, while an Epic Launcher client has a non-zero changelist (e.g., `47537391`). See [Network version compatibility](#network-version-compatibility-mixed-builds) for the workaround.

### `SetAutonomousProxy called on a unreplicated actor` spam (proxy)

The proxy logs this warning when a backend game server disconnects or crashes while a player is mid-migration. The proxy still holds references to a `PlayerController` that was created during the migration handoff but never fully replicated. The warning is cosmetic — the proxy will clean up the orphaned connection when it detects the backend disconnect via `DetectGameServerDisconnections()`.

---

## Known Issues & Required Engine Patches

The DSTM migration pipeline has been **tested and verified working** with the engine source patches documented below. Without these patches, the engine crashes when processing migrated remote objects. The migration data itself serializes and transfers correctly via the plugin.

> **AutoRTFM context:** UE 5.7's DSTM framework relies heavily on AutoRTFM transactional memory. AutoRTFM requires Epic's proprietary `verse-clang` compiler, which is not available for MSVC builds. With MSVC, `AutoRTFM::IsClosed()` always returns `false` and `Transact()` is a no-op. The patches below guard all AutoRTFM-dependent code paths so they degrade gracefully instead of crashing.

### Patch 1: `RemoteObjectTransfer.cpp` — Null `ActiveRequest` guard

**File:** `Engine/Source/Runtime/CoreUObject/Private/UObject/RemoteObjectTransfer.cpp`

**Symptom:** Source server crashes with `GTransferQueue.ActiveRequest` assertion during `ServerReplicateActors` after a migration.

**Fix:** In `ResolveObject()`, return gracefully when `GTransferQueue.ActiveRequest` is `nullptr` instead of asserting:

```cpp
if (!GTransferQueue.ActiveRequest)
{
    // Outside of a DSTM transaction — stale FWeakObjectPtr reference.
    // Return the object as-is without attempting remote migration.
    return;
}
```

### Patch 2: `RemoteObject.cpp` — AutoRTFM guard before `MigrateObjectFromRemoteServer`

**File:** `Engine/Source/Runtime/CoreUObject/Private/UObject/RemoteObject.cpp`

**Fix:** Guard `TryResolveRemoteObject()` to skip migration when AutoRTFM is not in a closed transaction:

```cpp
if (!AutoRTFM::IsClosed())
{
    return nullptr; // Cannot migrate outside closed transaction (MSVC)
}
```

### Patch 3: `RemoteExecutor.cpp` — `AbortTransactionRequiresDependencies` guard

**File:** `Engine/Source/Runtime/CoreUObject/Private/UObject/RemoteExecutor.cpp`

**Fix:** Return gracefully from `AbortTransactionRequiresDependencies()` when AutoRTFM is unavailable.

### Patch 4: `DataReplication.cpp` — `ensureMsgf` downgrade

**File:** `Engine/Source/Runtime/Engine/Private/DataReplication.cpp`

**Symptom:** `ensureMsgf(false, "ExecutePendingTransactionalRPCs ...")` fires as a fatal assertion in Debug/Development builds.

**Fix:** Replace the `ensureMsgf` with a one-time `UE_LOG(Warning)` so the server logs the condition but does not crash.

### Patch 5: `PlayerController.cpp` — Null `PlayerCameraManager` guard

**File:** `Engine/Source/Runtime/Engine/Private/PlayerController.cpp`

**Symptom:** Migrated PC ticks with a null `PlayerCameraManager` reference.

**Fix:** Add `IsValid(PlayerCameraManager)` guard before accessing it.

### Patch 6: `PlayerCameraManager.cpp` — Null `this` guard in `GetViewTargetPawn`

**File:** `Engine/Source/Runtime/Engine/Private/PlayerCameraManager.cpp`

**Fix:** Guard `GetViewTargetPawn()` against being called on a null or pending-kill instance.

### Patches already documented above

- **`TrackInstancePropertyBindings.h`** — `FVolatilePropertyStep` size assertion (see [Engine Build Requirement](#engine-build-requirement))
- **`SceneQuery.cpp`** — unreachable code fix (see [Engine Build Requirement](#engine-build-requirement))

### Summary of all required engine patches

| File | Patch | Purpose |
|------|-------|---------|
| `TrackInstancePropertyBindings.h` | Size assert conditional | `FWeakObjectPtr` grows from 8→16 bytes |
| `SceneQuery.cpp` | Unreachable code fix | MSVC C4702 with warnings-as-errors |
| `RemoteObjectTransfer.cpp` | Null `ActiveRequest` guard | Prevents crash on stale weak pointers post-migration |
| `RemoteObject.cpp` | AutoRTFM closed check | Skips migration when not in closed transaction |
| `RemoteExecutor.cpp` | AutoRTFM availability guard | Graceful return without verse-clang |
| `DataReplication.cpp` | `ensureMsgf` → warning log | Non-fatal logging for transactional RPC edge case |
| `PlayerController.cpp` | Null camera manager guard | Migrated PC may tick before camera is ready |
| `PlayerCameraManager.cpp` | Null `this` guard | Prevents crash on destroyed camera manager |

---

## License

This plugin is a derivative of Epic Games' MultiServerReplication plugin from Unreal Engine 5.7. See [LICENSE.md](https://github.com/EpicGames/UnrealEngine/blob/release/LICENSE.md) for the Unreal Engine license terms.
