# Remote Debug System

Local RemoteDebug queue and dispatch plumbing. It is not a shipped remote-control feature.

**Source:** `SparkEngine/Source/Engine/RemoteDebug/RemoteDebugSystem.h`

## Overview

SparkEngine does **not** ship a RemoteDebug listener, socket implementation,
network transport, credential protocol, or remote-administration service. The
classes here retain local queue and dispatch plumbing for editor integration and
testing only.

`StartServer(port)` and `ConnectToTarget(address, port)` record logical state
for a future, separately reviewed transport. They do not bind a port, open a
socket, establish a connection, authenticate a peer, or make a remote endpoint
available. A status such as `logical-listen` or `connecting` is an intent
record, not a listener or transport.

`RemoteSession::EnqueueReceived()` is a public raw transport-adapter hook, not
an authentication API. It carries no principal, so the server dispatches it as
anonymous and returns `{"error":"access_denied"}` before any handler runs.
The same rule applies to public `RemoteDebugServer::ProcessCommand()` calls.
No shipped adapter can attach a principal to either path.

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
  +-- Local loopback pump (client send -> server recv, server send -> client recv)
```

### Message Flow

```
Local Client                       Local Server
     |                                  |
     |-- EnqueueSend(cmd) ------------>|
     |   [in-process loopback only]    |
     |                                 |-- authorize -> handler -> audit
     |                                 |-- EnqueueSend(response)
     |<-- PollResponses() -------------|
```

There is no network-mode message flow. `RemoteCommand` carries type, payload,
request ID, and timestamp only; it never serializes credentials, identity,
roles, or capabilities.

## Key Classes

| Class | Description |
|-------|-------------|
| `RemoteDebugSystem` | Singleton owning server and client instances |
| `RemoteDebugServer` | Logical local server state and fail-closed dispatch |
| `RemoteDebugClient` | Local request queue and convenience methods |
| `RemoteSession` | Thread-safe local send/receive queues; no transport |
| `RemoteCommand` | In-memory message; no identity or credentials |

## Usage

### Loopback Mode (Local Inspection)

```cpp
auto& debug = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
debug.Initialize();
debug.EnableLoopback();  // In-process queues; no sockets or transport

// Observer-only local inspection is permitted.
auto* client = debug.GetClient();
uint32_t reqId = client->GetProperty("player.health");

// Update pumps loopback and processes commands
debug.Update(0.016f);

// Poll responses
auto responses = client->PollResponses();
for (const auto& resp : responses)
{
    // resp.type == "property_value"
    // resp.payload contains the local inspection result
}
```

`ExecuteConsoleCommand()` and `SetProperty()` are intentionally denied in
normal public loopback. They return `{"error":"access_denied"}` and must not
run an engine console command or mutate a property.

### Logical future-transport state

```cpp
auto& debug = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
debug.Initialize();

// These only record intent. They do not create a listener, socket,
// authentication handshake, or remote connection.
debug.StartServer(9090);
debug.ConnectToTarget("192.168.1.100", 9090);
```

### Custom Command Handlers

Custom handlers must name the least privilege capability they need. A handler
without an explicit capability defaults to console-execution authority and is
therefore denied to public loopback.

```cpp
auto* server = debug.GetServer();
server->RegisterCommandHandler("local_inspect", Spark::RemoteDebug::RemoteDebugCapability::Inspect,
    [](const Spark::RemoteDebug::RemoteCommand& cmd) {
        // Return local inspection data; do not expose a remote control path.
        return Spark::RemoteDebug::RemoteCommand{
            "local_inspect_result", R"({"status":"ok"})", cmd.requestId, 0.0f
        };
    });
```

## API Reference

### RemoteDebugSystem

| Method | Description |
|--------|-------------|
| `Initialize() / Shutdown()` | Lifecycle management |
| `StartServer(port)` | Record logical listen state; no listener or socket is created |
| `ConnectToTarget(addr, port)` | Record connection intent; no transport or handshake exists |
| `EnableLoopback()` | In-process observer-only queue bridge; no sockets or authority escalation |
| `Update(float dt)` | Pump local queues and process authorized local inspection commands |
| `IsConnected()` | True for enabled local loopback or logical connected state only |

### RemoteDebugClient

| Method | Description |
|--------|-------------|
| `ExecuteConsoleCommand(cmd)` | Queues a request; public loopback denies it before console execution |
| `GetProperty(path)` | Request a local observer-only property value |
| `SetProperty(path, value)` | Queues a request; public loopback denies mutation |
| `RequestPerformanceSnapshot()` | Request local observer-only CPU/GPU/memory stats |
| `PollResponses()` | Drain local queue responses since the last poll |

### Built-in Command Types

| Type | Description |
|------|-------------|
| `console_cmd` | Requires console-execution capability; public loopback denies it |
| `property_get` | Observer local inspection, returns `property_value` |
| `property_set` | Requires mutation capability; public loopback denies it |
| `profile_data` | Observer local performance snapshot |
| `heartbeat` | Observer local liveness response |

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| Logical port | 9090 | Recorded future-adapter intent; no TCP listener exists |
| Loopback mode | off | Enable observer-only in-process local inspection |

## Dispatch and revocation

The server validates command shape, expiration, replay order, rate limits, and
required capability before invoking a handler. Audit entries are bounded and
omit payloads, credentials, and grants.

`StopListening()` takes an exclusive execution lease. An already-authorized
protected handler completes and records its `Allowed` outcome before
`StopListening()` returns; after return, all principals from that epoch are
revoked and cannot cause another protected effect. This is synchronization,
not a post-hoc audit correction.

## What would be required before remote use

Remote Debug remains unavailable for remote administration until a future
change supplies an authenticated transport, credential enrollment and rotation,
peer identity binding, secure key storage, protocol validation, wire-boundary
replay and rate tests, authorization review, and an operational rollout plan.
Adding a socket alone would be unsafe and is explicitly out of scope for this
subsystem.

## Related Systems

- [Console System](SparkConsole.md) -- trusted in-engine console; not exposed by public loopback
- [Profiler](../advanced/Profiler-and-Debugging.md) -- Performance monitoring data source
- [Editor](SparkEditor.md) -- editor-side local inspection UI
