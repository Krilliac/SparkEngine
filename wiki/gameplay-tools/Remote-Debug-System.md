# Remote Debug System

Bidirectional command channel for inspecting and modifying a running game from the editor, with loopback mode for local testing.

**Source:** `SparkEngine/Source/Engine/RemoteDebug/RemoteDebugSystem.h`

## Overview

The Remote Debug System provides a live link between the editor and a running game instance. The editor sends commands (console commands, property reads/writes, performance requests) to the game, and the game dispatches them to registered handlers and returns responses. This enables live tuning, inspection, and diagnostics without stopping the game.

Transport is abstracted behind the `RemoteSession` class, which provides thread-safe send/receive queues. A real transport adapter (TCP socket) plugs into `EnqueueReceived()` and `DequeuePendingSend()`. For testing and single-machine workflows, `EnableLoopback()` connects server and client through shared queues with no sockets required.

The `RemoteDebugServer` runs in the game runtime with built-in handlers for console commands, property get/set, performance data, and heartbeat. Custom handlers can be registered for game-specific debug commands.

## Architecture

```
RemoteDebugSystem (singleton)
  +-- RemoteDebugServer (game-side)
  |     +-- RemoteSession (thread-safe queues)
  |     +-- CommandHandler map (type -> callback)
  |     +-- Built-in handlers: console_cmd, property_get/set, profile_data, heartbeat
  +-- RemoteDebugClient (editor-side)
  |     +-- RemoteSession (thread-safe queues)
  |     +-- Convenience methods (ExecuteConsoleCommand, GetProperty, etc.)
  +-- Loopback pump (client send -> server recv, server send -> client recv)
```

### Message Flow

```
Editor (Client)                    Game (Server)
     |                                  |
     |-- SendCommand(cmd) ------------>|
     |   [EnqueueSend -> transport ->  |
     |    EnqueueReceived]             |
     |                                 |-- ProcessCommand(cmd)
     |                                 |-- handler(cmd) -> response
     |                                 |-- EnqueueSend(response)
     |<-- PollResponses() -------------|
```

## Key Classes

| Class | Description |
|-------|-------------|
| `RemoteDebugSystem` | Singleton owning server and client instances |
| `RemoteDebugServer` | Accepts connections, dispatches commands to handlers |
| `RemoteDebugClient` | Connects to game, provides convenience debug methods |
| `RemoteSession` | Thread-safe connection state with send/receive queues |
| `RemoteCommand` | Wire message with type, JSON payload, request ID, timestamp |

## Usage

### Loopback Mode (Testing)

```cpp
auto& debug = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
debug.Initialize();
debug.EnableLoopback();  // No sockets needed

// Send a command from the client side
auto* client = debug.GetClient();
uint32_t reqId = client->ExecuteConsoleCommand("stat fps");

// Update pumps loopback and processes commands
debug.Update(0.016f);

// Poll responses
auto responses = client->PollResponses();
for (const auto& resp : responses)
{
    // resp.type == "console_cmd_result"
    // resp.payload contains JSON result
}
```

### Network Mode

```cpp
auto& debug = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
debug.Initialize();

// Game side: start listening
debug.StartServer(9090);

// Editor side: connect to game
debug.ConnectToTarget("192.168.1.100", 9090);
```

### Custom Command Handlers

```cpp
auto* server = debug.GetServer();
server->RegisterCommandHandler("spawn_entity",
    [](const Spark::RemoteDebug::RemoteCommand& cmd) {
        // Parse cmd.payload, spawn entity
        return Spark::RemoteDebug::RemoteCommand{
            "spawn_result", R"({"status":"ok","entityId":42})", cmd.requestId, 0.0f
        };
    });
```

## API Reference

### RemoteDebugSystem

| Method | Description |
|--------|-------------|
| `Initialize() / Shutdown()` | Lifecycle management |
| `StartServer(port)` | Begin listening for editor connections |
| `ConnectToTarget(addr, port)` | Connect client to a running game |
| `EnableLoopback()` | Connect server and client in-process (no sockets) |
| `Update(float dt)` | Per-frame update: pump loopback, process commands |
| `IsConnected()` | True if either side has an active connection |

### RemoteDebugClient

| Method | Description |
|--------|-------------|
| `ExecuteConsoleCommand(cmd)` | Run a console command on the remote game |
| `GetProperty(path)` | Request a dot-separated property value |
| `SetProperty(path, value)` | Set a property on the remote game |
| `RequestPerformanceSnapshot()` | Request CPU/GPU/memory stats |
| `PollResponses()` | Drain all received responses since last poll |

### Built-in Command Types

| Type | Description |
|------|-------------|
| `console_cmd` | Execute a console command, returns `console_cmd_result` |
| `property_get` | Read a property by path, returns `property_value` |
| `property_set` | Write a property, returns `property_set_result` |
| `profile_data` | Performance snapshot (FPS, CPU, GPU, memory) |
| `heartbeat` | Connection keepalive check |

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Server port | 9090 | TCP port for editor connections |
| Loopback mode | off | Enable with `EnableLoopback()` for local testing |

## Related Systems

- [Console System](SparkConsole.md) -- In-engine console for command execution
- [Profiler](../advanced/Profiler-and-Debugging.md) -- Performance monitoring data source
- [Editor](SparkEditor.md) -- Editor-side UI for remote debugging
