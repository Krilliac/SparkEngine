# Dedicated Server

SparkEngine supports two approaches for running a dedicated server: a **built-in dedicated server** compiled into the engine core, and a **game module dedicated server** loaded as a DLL plugin at runtime. This page compares both approaches and helps you choose the right one for your project.

For MMO-scale multiplayer with multiple server processes managing different world areas, see [Area Server Architecture](Area-Server-Architecture).

**Source:** `SparkEngine/Source/Engine/Networking/DedicatedServer.h`, `SparkEngine/Source/Engine/Networking/NetworkManager.h`

> **Note:** Both approaches require `ENABLE_NETWORKING=ON` during CMake configuration. See [Networking](Networking) for full networking documentation.

## Architecture Overview

### Game Module Approach

The game module approach uses the engine's [IModule plugin system](Creating-a-Game-Module) to run the server. The dedicated server is a DLL loaded by the engine executable at runtime, with a `-headless` flag disabling graphics and audio.

```
┌─────────────────────────────────┐
│     SparkEngine.exe             │
│         -headless               │
│                                 │
│  ┌───────────────────────────┐  │
│  │  Full Engine Init         │  │
│  │  (Graphics stubbed)       │  │
│  └───────┬───────────────────┘  │
│          │ LoadLibrary()        │
│  ┌───────▼───────────────────┐  │
│  │  SparkGame.dll            │  │
│  │  (Server game logic)      │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

- Engine executable is shared between editor, client, and server
- Server game logic lives in a DLL implementing `Spark::IModule`
- `-headless` flag disables rendering at runtime (subsystems still initialized)
- `NetworkManager` accessed via `EngineContext` service locator

### Built-in Dedicated Server Approach

The built-in approach compiles server functionality directly into a dedicated executable (or engine build variant), skipping unnecessary subsystems entirely at compile time.

```
┌─────────────────────────────────┐
│     SparkServer.exe             │
│                                 │
│  ┌───────────────────────────┐  │
│  │  Selective Init           │  │
│  │  (No graphics/audio)      │  │
│  └───────┬───────────────────┘  │
│          │                      │
│  ┌───────▼───────────────────┐  │
│  │  DedicatedServer          │  │
│  │  (Statically linked)      │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

- Separate executable with only the subsystems a server needs
- No DLL loading -- server logic is statically linked
- Can omit `GraphicsEngine`, `InputManager`, `AudioEngine` at compile time
- Tighter integration with `NetworkManager`, `PhysicsSystem`, and ECS

## Comparison

| Aspect | Game Module | Built-in Server |
|--------|-------------|-----------------|
| **Entry point** | `SparkEngine.exe -headless` | Dedicated `SparkServer.exe` |
| **Module loading** | Dynamic DLL via `CreateModule()` | Static linking |
| **Engine init** | Full engine init (graphics stubbed) | Selective init (skip graphics entirely) |
| **Game logic** | Via `IModule::OnUpdate()` | Direct engine API access |
| **Hot reload** | Yes -- reload DLL without restart | No -- requires full rebuild |
| **Binary size** | Larger (includes graphics stubs) | Smaller (graphics omitted) |
| **Startup time** | Longer (full init + DLL load) | Faster (minimal init) |
| **Memory footprint** | Higher (all subsystems initialized) | Lower (only server subsystems) |
| **Scaling** | Good for small teams | Better for large deployments |
| **Rebuild required** | Only the DLL | Full engine rebuild |

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
| `lanOnly` | `bool` | `false` | Restrict to LAN connections only |

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
| `rconPassword` | `string` | `""` | RCON password (empty = disabled) |
| `rconPort` | `uint16_t` | `0` | RCON port (0 = game port + 1) |
| `enableLogging` | `bool` | `true` | Write server log file |
| `logFilePath` | `string` | `"server.log"` | Path to the log file |

### LAN Discovery Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enableLanBroadcast` | `bool` | `true` | Broadcast server on LAN |
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

### Game Module

```cpp
// Launch: SparkEngine.exe -headless -game SparkGame.dll

// 1. Engine initializes all subsystems (graphics stubbed in headless mode)
// 2. Engine loads SparkGame.dll via LoadLibrary/dlopen
// 3. Engine calls CreateModule() -> IModule*
// 4. Engine calls OnLoad(context) with full EngineContext
// 5. Main loop calls OnUpdate(deltaTime) each frame
// 6. NetworkManager available via context, already initialized
```

### Built-in Server

```cpp
// Launch: SparkServer.exe --port 27015 --maxclients 32

int main()
{
    EngineContext ctx;
    // Only register what the server needs
    ctx.RegisterSubsystem<Spark::Net::NetworkManager>();
    ctx.RegisterSubsystem<PhysicsSystem>();
    ctx.RegisterSubsystem<ECSManager>();
    // Skip: GraphicsEngine, InputManager, AudioEngine

    auto& net = Spark::Net::NetworkManager::GetInstance();
    net.Initialize();
    net.StartServer(27015, 32);

    // Server main loop
    while (running)
    {
        float dt = timer.GetDeltaTime();
        net.Update(dt);
        physics.StepSimulation(dt);
        ecs.Update(dt);
    }

    net.Shutdown();
}
```

### Using DedicatedServer Class

The `DedicatedServer` class provides a higher-level API with tick loop management, map rotation, RCON, and LAN discovery:

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
config.rconPassword = "mySecretPassword";
config.enableLanBroadcast = true;

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
// Main thread can handle RCON, admin commands, etc.
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

### RCON (Remote Console)

| Method | Description |
|--------|-------------|
| `void RegisterRconCommand(name, description, handler)` | Register a custom RCON command |
| `string ExecuteRcon(const string& commandLine)` | Execute an RCON command string |
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

## RCON Commands

### Built-in RCON Commands

The server registers these RCON commands automatically:

| Command | Description | Example |
|---------|-------------|---------|
| `help` | List all RCON commands | `help` |
| `status` | Show server status | `status` |
| `kick` | Kick a player by ID | `kick 3 cheating` |
| `ban` | Ban a player by ID | `ban 5 exploit` |
| `map` | Change the current map | `map dm_arena` |
| `say` | Broadcast a message to all players | `say Server restarting in 5 minutes` |

### Custom RCON Commands

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

LAN broadcasting is automatic when `enableLanBroadcast = true`. The server periodically sends a `ServerBroadcastInfo` packet on the configured UDP port (default 27016).

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

### Use the Built-in Dedicated Server If

- You are deploying to **production** at scale (many server instances)
- You need **optimal performance** with minimal overhead
- You want **compile-time elimination** of unused subsystems (smaller binary)
- You operate **dedicated server infrastructure** (cloud fleets, containers)
- You need **server-specific optimizations** (higher tick rate, custom scheduling)

## Headless Operation

Both approaches can run without a display, but they differ in how they achieve it:

| Aspect | Game Module | Built-in Server |
|--------|-------------|-----------------|
| **Graphics** | Disabled at runtime (`-headless` flag) | Omitted at compile time |
| **Audio** | Disabled at runtime | Omitted at compile time |
| **Input** | Disabled at runtime | Omitted at compile time |
| **Physics** | Active | Active |
| **Networking** | Active (via `ENABLE_NETWORKING`) | Active (always compiled in) |
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

### Built-in Server Build

```bash
# Separate build target for the dedicated server
cmake -B build -DENABLE_NETWORKING=ON -DENABLE_GRAPHICS=OFF -DBUILD_DEDICATED_SERVER=ON
cmake --build build --config Release --target SparkServer

# Run the dedicated server
./build/bin/SparkServer --port 27015 --maxclients 32
```

## Thread Safety

The `DedicatedServer` class uses internal synchronization for safe multi-threaded access:

| Resource | Protection | Notes |
|----------|-----------|-------|
| Server running state | `std::atomic<bool>` | Lock-free read from any thread |
| RCON commands | `std::mutex` | Safe to register/execute from any thread |
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

See [Networking](Networking) for full details on these systems.

## Migration Between Approaches

### From Game Module to Built-in

If you start with the game module approach during development and want to switch to built-in for production:

1. Move game logic from `IModule::OnUpdate()` into the server's main loop
2. Replace `IEngineContext*` calls with direct subsystem access
3. Remove DLL export macros (`SPARK_IMPLEMENT_MODULE`)
4. Add a `SparkServer` build target in CMake
5. Configure the build to exclude graphics/audio subsystems

### From Built-in to Game Module

If you want to add hot-reload support during development:

1. Extract server game logic into a class implementing `Spark::IModule`
2. Add DLL exports via `SPARK_IMPLEMENT_MODULE`
3. Access subsystems through `IEngineContext*` instead of directly
4. Run via `SparkEngine.exe -headless -game YourServer.dll`

---

## See Also

- [Networking](Networking) -- Full networking system documentation
- [Creating a Game Module](Creating-a-Game-Module) -- IModule interface and DLL setup
- [Architecture Overview](Architecture-Overview) -- Engine architecture and subsystems
- [Entity Component System](Entity-Component-System) -- NetworkIdentity component
- [Build System and CMake Modules](Build-System-and-CMake-Modules) -- Build configuration
- [Gameplay Systems](Gameplay-Systems) -- Multiplayer game modes
