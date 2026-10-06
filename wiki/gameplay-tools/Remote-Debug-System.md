# Remote Debug System

Local RemoteDebug queue and dispatch plumbing. It is not a shipped remote-control feature.

**Source:** `SparkEngine/Source/Engine/RemoteDebug/RemoteDebugSystem.h`

## Overview

SparkEngine does **not** ship a RemoteDebug listener, socket implementation,
network transport, credential protocol, or remote-administration service. The
classes here retain local queue and dispatch plumbing for editor integration and
testing only.

**Remote administration is permanently unavailable in stable-v1** (owner
decision OD-05, work item SEC-100). There is no remote entry point to enable:
`RemoteDebugSystem::StartServer(port)`, `ConnectToTarget(address, port)` and
`RemoteDebugClient::Connect(address, port)` were removed, and
`RemoteDebugServer::StartListening()` takes no port. It only starts a new local
in-process authority epoch. No configuration value or command-line switch adds
a listener. `Tests/TestSEC100RemoteAdminUnavailableReal.cpp` fails the build
if any of those entry points returns.

`RemoteSession::EnqueueReceived()` is a public raw queue call, not an
authentication API. It carries no principal, so the server dispatches it as
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
| `RemoteDebugServer` | Local in-process authority epoch and fail-closed dispatch |
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

### Custom Command Handlers

Custom handlers must name the least privilege capability they need. A handler
without an explicit capability defaults to console-execution authority and is
therefore denied to public loopback.

Built-in command names (`console_cmd`, `property_get`, `property_set`,
`profile_data`, and `heartbeat`) are reserved and cannot be rebound through
the public registration API. This keeps a custom handler from weakening a
built-in command's authority requirement.

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
| `EnableLoopback()` | In-process observer-only queue bridge; no sockets or authority escalation |
| `Update(float dt)` | Pump local queues and process authorized local inspection commands |
| `IsConnected()` | True only while local loopback is enabled |

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

## Remote use is unavailable

Per OD-05 no authenticated remote channel is built for stable-v1, so Remote
Debug stays local only. Any future remote use would need a new owner decision
and its own reviewed work: authenticated transport, credential enrollment and
rotation, peer identity binding, secure key storage, protocol validation,
wire-boundary replay and rate tests, and authorization review. Adding a socket
alone would be unsafe.

## Related Systems

- [Console System](SparkConsole.md) -- trusted in-engine console; not exposed by public loopback
- [Profiler](../advanced/Profiler-and-Debugging.md) -- Performance monitoring data source
- [Editor](SparkEditor.md) -- editor-side local inspection UI
