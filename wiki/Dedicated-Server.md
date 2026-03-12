# Dedicated Server

SparkEngine supports two approaches for running a dedicated server: a **built-in dedicated server** compiled into the engine core, and a **game module dedicated server** loaded as a DLL plugin at runtime. This page compares both approaches and helps you choose the right one for your project.

**Source:** `SparkEngine/Source/Engine/Networking/NetworkManager.h`

> **Note:** Both approaches require `ENABLE_NETWORKING=ON` during CMake configuration. See [Networking](Networking) for full networking documentation.

## Architecture Overview

### Game Module Approach

The game module approach uses the engine's [IModule plugin system](Creating-a-Game-Module) to run the server. The dedicated server is a DLL loaded by the engine executable at runtime, with a `-headless` flag disabling graphics and audio.

```
┌─────────────────────────────┐
│     SparkEngine.exe         │
│         -headless           │
│                             │
│  ┌───────────────────────┐  │
│  │  Full Engine Init     │  │
│  │  (Graphics stubbed)   │  │
│  └───────┬───────────────┘  │
│          │ LoadLibrary()    │
│  ┌───────▼───────────────┐  │
│  │  SparkGame.dll        │  │
│  │  (Server game logic)  │  │
│  └───────────────────────┘  │
└─────────────────────────────┘
```

- Engine executable is shared between editor, client, and server
- Server game logic lives in a DLL implementing `Spark::IModule`
- `-headless` flag disables rendering at runtime (subsystems still initialized)
- `NetworkManager` accessed via `EngineContext` service locator

### Built-in Dedicated Server Approach

The built-in approach compiles server functionality directly into a dedicated executable (or engine build variant), skipping unnecessary subsystems entirely at compile time.

```
┌─────────────────────────────┐
│     SparkServer.exe         │
│                             │
│  ┌───────────────────────┐  │
│  │  Selective Init       │  │
│  │  (No graphics/audio)  │  │
│  └───────┬───────────────┘  │
│          │                  │
│  ┌───────▼───────────────┐  │
│  │  Server Logic         │  │
│  │  (Statically linked)  │  │
│  └───────────────────────┘  │
└─────────────────────────────┘
```

- Separate executable with only the subsystems a server needs
- No DLL loading — server logic is statically linked
- Can omit `GraphicsEngine`, `InputManager`, `AudioEngine` at compile time
- Tighter integration with `NetworkManager`, `PhysicsSystem`, and ECS

## Comparison

| Aspect | Game Module | Built-in Server |
|--------|-------------|-----------------|
| **Entry point** | `SparkEngine.exe -headless` | Dedicated `SparkServer.exe` |
| **Module loading** | Dynamic DLL via `CreateModule()` | Static linking |
| **Engine init** | Full engine init (graphics stubbed) | Selective init (skip graphics entirely) |
| **Game logic** | Via `IModule::OnUpdate()` | Direct engine API access |
| **Hot reload** | Yes — reload DLL without restart | No — requires full rebuild |
| **Binary size** | Larger (includes graphics stubs) | Smaller (graphics omitted) |
| **Startup time** | Longer (full init + DLL load) | Faster (minimal init) |
| **Memory footprint** | Higher (all subsystems initialized) | Lower (only server subsystems) |
| **Scaling** | Good for small teams | Better for large deployments |
| **Rebuild required** | Only the DLL | Full engine rebuild |

## Initialization Flow

### Game Module

```cpp
// Launch: SparkEngine.exe -headless -game SparkGame.dll

// 1. Engine initializes all subsystems (graphics stubbed in headless mode)
// 2. Engine loads SparkGame.dll via LoadLibrary/dlopen
// 3. Engine calls CreateModule() → IModule*
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

- [Networking](Networking) — Full networking system documentation
- [Creating a Game Module](Creating-a-Game-Module) — IModule interface and DLL setup
- [Architecture Overview](Architecture-Overview) — Engine architecture and subsystems
- [Entity Component System](Entity-Component-System) — NetworkIdentity component
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — Build configuration
- [Gameplay Systems](Gameplay-Systems) — Multiplayer game modes
