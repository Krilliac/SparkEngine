# Dedicated Server

The repository contains two development host paths for dynamically loaded game
modules: the shared `SparkEngine -headless` runtime and the separate
`SparkServer` process. Both drive `IModule` callbacks through `ModuleManager`;
`SparkServer` adds dedicated health, stop, and gateway-control surfaces. The
Windows 11 x64 shared no-render host is inside the single-player, service-free
release profile, but it remains blocked and uncertified while explicit NullRHI
wiring and headless automation evidence are open. The networked dedicated-server
use case, `SparkServer`, and non-Windows headless hosts are outside that profile;
no production deployment or transport-security certification is claimed.

For MMO-scale multiplayer with multiple server processes managing different world areas, see [Area Server Architecture](Area-Server-Architecture.md).

**Source:** `SparkEngine/Source/Engine/Networking/DedicatedServer.h`,
`SparkEngine/Source/Core/SparkEngineWindowsHeadless.cpp`,
`SparkEngine/Source/Core/SparkEngineLinuxHeadless.cpp`,
`SparkServer/src/ServerApplication.cpp`

> **Note:** The `SparkServer` target and gameplay-networking services require
> `ENABLE_NETWORKING=ON`. The shared headless host can still execute a module
> without networking. See [Networking](Networking.md) for networking details.

> **Security status:** The gameplay UDP path is experimental, unauthenticated, and unencrypted. Servers bind to IPv4 loopback by default. Isolated LAN development requires one canonical RFC1918 interface/prefix such as `192.168.1.20/24`; the full subnet must remain private, exact network/broadcast addresses fail, and admitted peers must be concrete hosts in that subnet. Gateway-managed `SparkServer` processes require loopback and reject conflicting LAN configuration. NET-100 remains open for this experimental surface outside `stable-v1`.

## Architecture Overview

### Shared SparkEngine Headless Host

This path starts the normal engine executable with `-headless` or `-dedicated`,
selects a dynamic DLL/SO game module, constructs the platform's headless runtime
state, and drives the module through `ModuleManager`. Initialization details are
platform-specific; the current Windows headless entry supplies null graphics and
input services, while the Linux headless entry initializes no graphics/audio path.

```
┌─────────────────────────────────┐
│     SparkEngine.exe             │
│         -headless               │
│                                 │
│  ┌───────────────────────────┐  │
│  │ Headless host setup       │  │
│  │ (platform-specific)       │  │
│  └───────┬───────────────────┘  │
│          │ ModuleManager        │
│  ┌───────▼───────────────────┐  │
│  │  SparkGame.dll            │  │
│  │  (Server game logic)      │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

- Uses the shared engine executable as the host process
- Loads selected server/game logic as a dynamic `Spark::IModule`
- Calls `InitializeAll`, `UpdateAll`, and `FixedUpdateAll` through `ModuleManager`
- Uses platform-specific headless initialization rather than a certified common host contract

### Separate SparkServer Process

The `SparkServer` approach builds a separate executable linked against the full `SparkEngineLib`. At runtime it constructs a headless `EngineContext` with graphics and input set to `nullptr`, initializes selected headless asset services, and dynamically loads the requested game module or manifest. The current target does not prove compile-time removal of graphics, audio, or input code.

```
┌─────────────────────────────────┐
│     SparkServer.exe             │
│                                 │
│  ┌───────────────────────────┐  │
│  │  Headless EngineContext   │  │
│  │  (graphics/input nullptr) │  │
│  └───────┬───────────────────┘  │
│          │ dynamic module       │
│  ┌───────▼───────────────────┐  │
│  │  ModuleManager +          │  │
│  │  DedicatedServer          │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

- Separate executable linked to the complete `SparkEngineLib` module-facing runtime
- Dynamically loads one game module or a module manifest
- Constructs graphics/input-null `EngineContext` state and selected headless asset services
- Does not currently instantiate `NullRHIDevice` or prove compile-time subsystem stripping (`HEAD-220`)

## Comparison

| Aspect | Shared headless host | `SparkServer` process |
|--------|-------------|-----------------|
| **Entry point** | `SparkEngine.exe -headless` | Dedicated `SparkServer.exe` |
| **Module loading** | Dynamic module via `CreateModule()` | Dynamic selected module or manifest |
| **Engine init** | Windows headless entry with graphics/input `nullptr` | Headless `EngineContext` with graphics/input `nullptr` plus selected asset services |
| **Game logic** | Dynamic `IModule` callbacks through `ModuleManager` | Dynamic `IModule` callbacks through `ModuleManager` |
| **Host process** | Shared `SparkEngine.exe` headless entry | Separate `SparkServer.exe` linked to full `SparkEngineLib` |
| **NullRHI wiring** | Not instantiated by the current headless entry | Not instantiated by the current server entry (`HEAD-220`) |
| **Service controls** | Shared engine lifecycle | Dedicated health/stop and gateway-facing control surfaces |
| **Performance claims** | No release-certified comparison | No release-certified comparison |

## ServerConfig Reference

The `Spark::Net::ServerConfig` struct controls all aspects of the dedicated server. Here is the complete field reference:

### Identity Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `serverName` | `string` | `"Spark Dedicated Server"` | Display name shown in server browsers |
| `motd` | `string` | `""` | Message of the day shown to connecting clients |

### Network Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `port` | `uint16_t` | `27015` | UDP game port |
| `maxClients` | `int` | `32` | Maximum simultaneous connections |
| `tickRate` | `float` | `60.0` | Server simulation ticks per second |
| `clientTimeoutSeconds` | `float` | `30.0` | Kick after N seconds of silence |
| `heartbeatIntervalSeconds` | `float` | `1.0` | Interval between heartbeat packets |
| `endpointPolicy` | `NetworkEndpointPolicy` | captured loopback default | Exact bind address and admitted peer scope |

### Game Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `gameMode` | `GameModeType` | `Deathmatch` | Active game mode |
| `customGameModeName` | `string` | `""` | Name for `GameModeType::Custom` |
| `scoreLimit` | `int` | `50` | Score to end the match |
| `timeLimitMinutes` | `float` | `15.0` | Match time limit in minutes |
| `roundCount` | `int` | `1` | Number of rounds per match |
| `friendlyFire` | `bool` | `false` | Whether teammates can damage each other |
| `autoBalanceTeams` | `bool` | `true` | Automatically balance team sizes |

### Map Rotation Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `mapRotation` | `vector<string>` | `{}` | Ordered list of map names to cycle through |
| `randomizeMapOrder` | `bool` | `false` | Shuffle the rotation order |

### Administration Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `rconPassword` | `string` | `""` | Reserved compatibility field; currently ignored |
| `rconPort` | `uint16_t` | `0` | Reserved compatibility field; currently ignored |
| `enableLogging` | `bool` | `true` | Write server log file |
| `logFilePath` | `string` | `"server.log"` | Path to the log file |

### LAN Discovery Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enableLanBroadcast` | `bool` | `false` | Opt into unauthenticated LAN discovery metadata |
| `lanBroadcastPort` | `uint16_t` | `27016` | UDP port for LAN discovery |

### Performance Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enableAntiCheat` | `bool` | `false` | Enable server-side anti-cheat |
| `replicationRate` | `float` | `20.0` | Entity replication Hz |
| `snapshotHistorySize` | `int` | `64` | Snapshots for lag compensation |

## GameModeType Enum

```cpp
enum class GameModeType : uint8_t
{
    Deathmatch = 0,      // Free-for-all, individual scoring
    TeamDeathmatch,      // Team-based, team scoring
    CaptureTheFlag,      // Flag capture objectives
    Domination,          // Control point capture
    SearchAndDestroy,    // Bomb plant/defuse
    FreeForAll,          // No teams, last man standing
    Custom               // User-defined game mode
};
```

## Initialization Flow

### Shared Headless Host

```cpp
// Launch: SparkEngine.exe -headless -game SparkGame.dll
// 1. Parse the headless/dedicated flag and enter the platform headless path.
// 2. Create the platform-specific headless context, world, and selected services.
// 3. Load the selected dynamic module through ModuleManager.
// 4. Call ModuleManager::InitializeAll(context).
// 5. Drive ModuleManager::UpdateAll and FixedUpdateAll in the headless loop.
// 6. Preflight shutdown callbacks, unload modules, and tear down owned services.
```

### SparkServer Process

```cpp
// Launch: SparkServer.exe --manifest spark.modules.json --port 27015 --max-clients 32

// 1. ParseServerOptions requires exactly one --module or --manifest selection.
// 2. Create EngineContext(nullptr, nullptr, timer, eventBus).
// 3. Initialize headless asset services, World, SaveSystem, and CoroutineScheduler.
// 4. Load the selected dynamic module(s), then call ModuleManager::InitializeAll.
// 5. Start Net::DedicatedServer and optional authenticated local gateway control.
// 6. Drive ModuleManager::UpdateAll and FixedUpdateAll at the configured tick rate.
// 7. Preflight module shutdown, stop services, unload modules, and reset the context.
```

### Using DedicatedServer Class

The `DedicatedServer` class provides a higher-level API with tick loop management, map rotation, trusted local administration commands, and LAN discovery. It does **not** currently expose a remote RCON transport:

```cpp
#include "Engine/Networking/DedicatedServer.h"

Spark::Net::DedicatedServer server;

// Configure
Spark::Net::ServerConfig config;
config.serverName = "My FPS Server";
config.port = 27015;
config.maxClients = 24;
config.tickRate = 64.0f;
config.gameMode = Spark::Net::GameModeType::TeamDeathmatch;
config.scoreLimit = 75;
config.timeLimitMinutes = 10.0f;
config.mapRotation = {"dm_arena", "dm_warehouse", "dm_rooftop"};
config.endpointPolicy = Spark::Net::NetworkEndpointPolicy::Loopback();
config.enableLanBroadcast = false;

// Set callbacks
Spark::Net::ServerCallbacks callbacks;
callbacks.onServerStarted = []() { LOG("Server started!"); };
callbacks.onClientConnected = [](Spark::Net::ClientID id, const std::string& name) {
    LOG("Player connected: " + name);
};
callbacks.onClientDisconnected = [](Spark::Net::ClientID id, const std::string& reason) {
    LOG("Player disconnected: " + reason);
};
callbacks.onMapChanged = [](const std::string& map) {
    LOG("Map changed to: " + map);
};
server.SetCallbacks(callbacks);

// Start (launches tick loop on background thread)
if (!server.Start(config))
{
    LOG_ERROR("Failed to start server");
    return 1;
}

// Server is now running on its own thread
// Trusted host/control thread can dispatch local admin commands.
while (server.IsRunning())
{
    std::string input;
    std::getline(std::cin, input);
    std::string response = server.ExecuteRcon(input);
    std::cout << response << std::endl;
}

server.Stop();
```

## DedicatedServer API Reference

### Lifecycle Methods

| Method | Description |
|--------|-------------|
| `bool Start(const ServerConfig& config)` | Start the server with background tick loop |
| `void Stop()` | Graceful shutdown: notify clients, flush, stop |
| `bool IsRunning() const` | Check if the server is active |
| `void Tick(float deltaTime)` | Single tick (for external loop driving) |
| `bool InitializeOnly(const ServerConfig& config)` | Initialize without starting tick loop |

### Map Management

| Method | Description |
|--------|-------------|
| `void ChangeMap(const string& mapName)` | Load a specific map |
| `void RotateToNextMap()` | Advance to the next map in rotation |
| `const string& GetCurrentMap() const` | Get current map name |

### Match State

| Method | Description |
|--------|-------------|
| `void StartMatch()` | Begin a new match on current map |
| `void EndMatch()` | End current match (triggers score tally) |
| `bool IsMatchInProgress() const` | Check if a match is active |
| `float GetMatchTimeRemaining() const` | Remaining match time in seconds |

### Player Management

| Method | Description |
|--------|-------------|
| `void KickPlayer(ClientID id, const string& reason)` | Kick a player |
| `void BanPlayer(ClientID id, const string& reason)` | Ban a player (session-only) |
| `vector<ClientInfo> GetConnectedClients() const` | Get all connected clients |
| `uint32_t GetPlayerCount() const` | Current player count |

### Local Administration Commands (legacy RCON API names)

`ExecuteRcon` is an in-process command dispatcher. Network chat is not an
administration transport, and `rconPassword`/`rconPort` do not enable one.
Remote callers require a separate authenticated transport before invoking it.

| Method | Description |
|--------|-------------|
| `void RegisterRconCommand(name, description, handler)` | Register a local admin command |
| `string ExecuteRcon(const string& commandLine)` | Dispatch a command from trusted host code |
| `const vector<RconCommand>& GetRconCommands() const` | List registered commands |

### LAN Discovery

| Method | Description |
|--------|-------------|
| `void StartLanBroadcast()` | Begin broadcasting on LAN |
| `void StopLanBroadcast()` | Stop LAN broadcasting |
| `static vector<ServerBroadcastInfo> DiscoverLanServers(port, timeout)` | Find LAN servers (client-side) |

## ServerStats

The `ServerStats` struct provides real-time server metrics:

```cpp
struct ServerStats
{
    float uptimeSeconds;           // Time since server started
    uint64_t totalTicksProcessed;  // Total ticks executed
    float averageTickMs;           // Average tick duration
    float peakTickMs;              // Peak tick duration
    uint32_t currentPlayers;       // Currently connected
    uint32_t peakPlayers;          // Peak concurrent players
    uint64_t totalBytesIn;         // Total bytes received
    uint64_t totalBytesOut;        // Total bytes sent
    uint32_t totalConnectionsServed; // Total connections since start
    float currentTickRate;         // Actual tick rate (may vary)
    std::string currentMap;        // Active map name
    int currentMapIndex;           // Position in rotation
    float matchTimeRemaining;      // Remaining match seconds
    int currentRound;              // Current round number
};
```

Access stats at any time:

```cpp
const auto& stats = server.GetStats();
LOG("Uptime: " + std::to_string(stats.uptimeSeconds) + "s");
LOG("Players: " + std::to_string(stats.currentPlayers) + "/" + std::to_string(config.maxClients));
LOG("Tick: " + std::to_string(stats.averageTickMs) + "ms avg");
```

## ServerCallbacks

Register callbacks to respond to server lifecycle events:

```cpp
struct ServerCallbacks
{
    std::function<void()> onServerStarted;
    std::function<void()> onServerStopped;
    std::function<void(ClientID, const std::string&)> onClientConnected;
    std::function<void(ClientID, const std::string&)> onClientDisconnected;
    std::function<void(const std::string&)> onMapChanged;
    std::function<void(const std::string&)> onChatMessage;
    std::function<void(const std::string&, const std::string&)> onRconCommand;
    std::function<void(const std::string&)> onLogMessage;
};
```

## Local Administration Commands

### Local Administration Commands

The server registers these local commands automatically:

| Command | Description | Example |
|---------|-------------|---------|
| `help` | List all administration commands | `help` |
| `status` | Show server status | `status` |
| `kick` | Kick a player by ID | `kick 3 cheating` |
| `ban` | Ban a player by ID | `ban 5 exploit` |
| `map` | Change the current map | `map dm_arena` |
| `say` | Broadcast a message to all players | `say Server restarting in 5 minutes` |
| `players` | List connected players | `players` |
| `endmatch` | End the current match | `endmatch` |
| `nextmap` | Rotate and start the next map | `nextmap` |

There is intentionally no built-in `quit` command: calling `Stop()` from the
server tick thread would self-join. The owning host must request shutdown and
call `Stop()` from its control thread.

### Custom Administration Commands

Register your own commands for game-specific administration:

```cpp
server.RegisterRconCommand("setmode", "Change game mode",
    [&](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: setmode <deathmatch|tdm|ctf>";
        // Change game mode logic...
        return "Game mode changed to " + args[0];
    });

server.RegisterRconCommand("restart", "Restart the current match",
    [&](const std::vector<std::string>&) -> std::string {
        server.EndMatch();
        server.StartMatch();
        return "Match restarted";
    });
```

## LAN Server Discovery

### Server Side

LAN discovery metadata is emitted only when the authoritative `enableLanBroadcast` option is true. Loopback mode uses local unicast; explicit LAN mode derives one directed broadcast from the validated CIDR prefix and never uses `255.255.255.255`. Terrafront discovery consumes the same active `NetworkManager` policy and advertisement flag rather than rereading environment configuration. Discovery is disabled by default, unauthenticated, and does not make gameplay transport secure.

```cpp
struct ServerBroadcastInfo
{
    std::string serverName;                        // Display name
    std::string mapName;                           // Current map
    GameModeType gameMode;                         // Active game mode
    uint16_t port;                                 // Game port
    int currentPlayers;                            // Player count
    int maxPlayers;                                // Max capacity
    float ping;                                    // Filled by client
};
```

### Client Side

Discover LAN servers using the static utility method:

```cpp
auto servers = Spark::Net::DedicatedServer::DiscoverLanServers(27016, 2000);
for (const auto& s : servers)
{
    LOG(s.serverName + " (" + s.mapName + ") " +
        std::to_string(s.currentPlayers) + "/" + std::to_string(s.maxPlayers));
}
```

## When to Use Each Approach

### Use the Game Module Approach If

- You are **actively developing** and need hot-reload of game logic
- You want a **single codebase** for editor, client, and server
- Your team is **small** and deployment is simple
- You need **console command integration** via `SimpleConsole`
- You want to **test server code** in the editor without a separate build

### Evaluate the SparkServer Process If

- You are evaluating a future many-instance deployment after security and operations gates pass
- You need a separate process boundary from the client/editor host
- You need explicit module/manifest selection and headless asset-service initialization
- You need the dedicated health/stop or gateway-facing control surfaces for integration testing

## Headless Operation

Both approaches can run without a display, but they differ in how they achieve it:

| Aspect | Shared headless host | `SparkServer` process |
|--------|-------------|-----------------|
| **Graphics** | Graphics service is `nullptr`; no `NullRHIDevice` instance (`HEAD-220`) | Graphics service is `nullptr`; no `NullRHIDevice` instance (`HEAD-220`) |
| **Audio** | Not initialized by the headless entry; engine code remains available | Not initialized by the server entry; full engine library remains linked |
| **Input** | `EngineContext` input is `nullptr` | `EngineContext` input is `nullptr` |
| **Physics** | Module/runtime dependent | Module/runtime dependent |
| **Networking** | Available when `ENABLE_NETWORKING=ON` | Target exists only when `ENABLE_NETWORKING=ON` |
| **ECS** | Active | Active |

## Build Configuration

### Game Module Build

```bash
# Single build produces both engine executable and game DLL
cmake -B build -DENABLE_NETWORKING=ON -DBUILD_TESTS=ON
cmake --build build --config Release

# Run as dedicated server
./build/bin/SparkEngine -headless -game SparkGame.dll
```

### SparkServer Process Build

```bash
# Separate build target for the dedicated server
cmake -B build -DENABLE_NETWORKING=ON -DENABLE_SERVER_PROCESSES=ON
cmake --build build --config Release --target SparkServer

# Run the dedicated server using the project's module manifest
./build/bin/SparkServer --manifest spark.modules.json --port 27015 --max-clients 32
```

`SparkServer` requires either `--manifest <path>` or `--module <game-library>`
so that it can select the server game module explicitly.
`ENABLE_GRAPHICS=OFF` is not used here because that option is currently inert;
`SparkServer` still links the full engine library and relies on runtime headless wiring.

## Thread Safety

The `DedicatedServer` class uses internal synchronization for safe multi-threaded access:

| Resource | Protection | Notes |
|----------|-----------|-------|
| Server running state | `std::atomic<bool>` | Lock-free read from any thread |
| Local administration commands | `std::mutex` | Safe to register/execute from trusted host threads |
| Ban list | `std::mutex` | Safe to kick/ban from any thread |
| Log output | `std::mutex` | Thread-safe logging |
| LAN broadcast | `std::atomic<bool>` | Broadcast runs on its own thread |
| Tick loop | Dedicated thread | Internal; use `Tick()` for external driving |

## Shared Networking Features

Regardless of which approach you use, the underlying `NetworkManager` provides the same networking capabilities:

- **UDP-based** client/server architecture on port `27015` (default)
- **Message channels**: `Unreliable`, `Reliable`, `ReliableOrdered`
- **Entity replication** via `ReplicatedEntity` at 20 Hz
- **Client-side prediction** with input history and server reconciliation
- **Lag compensation** with 1-second hitbox rewinding via `LagCompensator`
- **Network statistics**: ping, jitter, packet loss, bandwidth
- **Serialization** via `NetBuffer` for efficient wire format
- **Thread safety**: queue mutex for message I/O and handler registration

See [Networking](Networking.md) for full details on these systems.

## Switching Host Processes

Both host paths require dynamic `IModule` game logic; switching hosts does not
mean moving logic into a statically linked server loop or replacing
`IEngineContext` with direct subsystem ownership.

1. Keep the game/server logic in a module exported with the repository's module ABI.
2. For the shared host, select it with `SparkEngine.exe -headless -game YourServer.dll`.
3. For the separate process, enable `ENABLE_SERVER_PROCESSES=ON` and pass the
   same module with `SparkServer.exe --module YourServer.dll` (or use a manifest).
4. Revalidate module dependencies against the different headless context and
   service set; neither host currently instantiates `NullRHIDevice` (`HEAD-220`).
5. Treat both paths as experimental until their security, operations, packaging,
   and exact-SHA release gates pass.

---

## See Also

- [Networking](Networking.md) -- Full networking system documentation
- [Creating a Game Module](../getting-started/Creating-a-Game-Module.md) -- IModule interface and DLL setup
- [Architecture Overview](../getting-started/Architecture-Overview.md) -- Engine architecture and subsystems
- [Entity Component System](Entity-Component-System.md) -- NetworkIdentity component
- [Build System and CMake Modules](../advanced/Build-System-and-CMake-Modules.md) -- Build configuration
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Multiplayer game modes
