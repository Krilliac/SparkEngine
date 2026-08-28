# Telemetry System

The Telemetry System provides opt-in event recording for gameplay analytics and performance metrics. Consent is reversible and revocation clears the queue, but a producer/revocation race described below remains open; this page does not claim an absolute concurrent consent guarantee.

**Source:** `SparkEngine/Source/Utils/Telemetry.h`

## Overview

| Class / Struct | Responsibility |
|---|---|
| `TelemetrySystem` | Singleton managing event recording, batching, auto-flush, and backend dispatch |
| `TelemetryConfig` | Configuration struct controlling enable state, consent, paths, batch sizes, and flush intervals |
| `TelemetryEvent` | A single telemetry data point with name, timestamp, properties, and session ID |
| `ITelemetryBackend` | Abstract interface for telemetry output destinations |
| `LocalFileTelemetryBackend` | Built-in backend that writes JSON files to disk |

## Key Enums and Types

### TelemetryEvent

```cpp
struct TelemetryEvent
{
    std::string name;                                        // Event name (e.g. "level_complete")
    uint64_t timestamp = 0;                                  // Epoch milliseconds
    std::unordered_map<std::string, std::string> properties; // Key-value metadata
    std::string sessionId;                                   // Session identifier
    uint64_t sequence = 0;                                   // In-memory flush ordering
};
```

### TelemetryConfig

```cpp
struct TelemetryConfig
{
    bool enabled = false;               // Master enable switch
    bool consentGiven = false;          // User has opted in
    std::string localExportPath;        // Directory for local JSON export
    std::string httpEndpoint;           // Remote endpoint URL (future use)
    uint32_t batchSize = 50;            // Events per flush batch
    float flushIntervalSeconds = 30.0f; // Auto-flush interval
    uint32_t maxQueueSize = 10000;      // Maximum queued events before dropping
};
```

## Quick Start

### Basic initialization and event recording

```cpp
#include "Utils/Telemetry.h"

auto& telemetry = Spark::TelemetrySystem::GetInstance();

Spark::TelemetryConfig cfg;
cfg.enabled = true;
cfg.consentGiven = true;
cfg.localExportPath = "telemetry/";
cfg.batchSize = 25;
cfg.flushIntervalSeconds = 60.0f;

telemetry.Initialize(cfg);

// Record a simple event
telemetry.RecordEvent("game_start");

// Record an event with properties
telemetry.RecordEvent("level_start", {
    {"level", "3"},
    {"difficulty", "hard"},
    {"character", "warrior"}
});

// Record a timed event (e.g., level load duration)
telemetry.RecordTimedEvent("level_load", 2345.6f, {
    {"level", "3"},
    {"assets_loaded", "142"}
});

// Shutdown flushes remaining events
telemetry.Shutdown();
```

### Per-frame update with auto-flush

```cpp
// In your main loop:
void GameLoop(float dt)
{
    // Auto-flushes when flushIntervalSeconds elapses
    telemetry.Update(dt);

    // Game logic that records events...
    if (playerDied)
    {
        telemetry.RecordEvent("player_death", {
            {"cause", "fall_damage"},
            {"position_x", std::to_string(pos.x)},
            {"position_y", std::to_string(pos.y)}
        });
    }
}
```

### Manual flush

```cpp
// Force an immediate flush (e.g., before a loading screen)
telemetry.FlushEvents();
```

### Querying system state

```cpp
const auto& config = telemetry.GetConfig();
uint32_t queued = telemetry.GetQueueSize();
bool hasConsent = telemetry.HasConsent();

Log::Info("Telemetry", "Enabled: {}, Consent: {}, Queued: {}/{}",
          config.enabled, hasConsent, queued, config.maxQueueSize);
```

## Configuration

### TelemetryConfig Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | `bool` | `false` | Master enable switch; must be true for any recording |
| `consentGiven` | `bool` | `false` | User opt-in flag; must be true for any recording |
| `localExportPath` | `std::string` | `""` | Directory for `LocalFileTelemetryBackend` JSON output |
| `httpEndpoint` | `std::string` | `""` | Remote HTTP endpoint URL (reserved for future use) |
| `batchSize` | `uint32_t` | `50` | Number of events per flush batch |
| `flushIntervalSeconds` | `float` | `30.0f` | Seconds between automatic flushes in `Update()` |
| `maxQueueSize` | `uint32_t` | `10000` | Maximum queued events; new events are silently dropped when full |

### Recording Prerequisites

All three conditions must be true for `RecordEvent()` to accept an event:

1. `m_initialized` -- `Initialize()` has been called
2. `m_config.enabled` -- master switch is on
3. `m_config.consentGiven` -- user has opted in

If any condition is observed false, events are silently dropped. This is the intended gate, but it does not close the concurrent producer/revocation race described below.

## Privacy and Consent API

The telemetry system enforces strict privacy controls:

### Setting consent at runtime

```cpp
// User opts in via a settings menu
telemetry.SetConsent(true);

// User opts out -- immediately clears the event queue
telemetry.SetConsent(false);
// All queued events are gone; no further events will be recorded
```

### Checking consent status

```cpp
if (telemetry.HasConsent())
{
    // Safe to show telemetry-related UI
}
```

### Current privacy behavior and limitation

- `RecordEvent()` and `RecordTimedEvent()` check `CanRecord()` before constructing an event.
- `SetConsent(false)` is reversible and clears the queue while holding the queue mutex.
- A producer can pass `CanRecord()` just before revocation and enqueue after the clear because enqueue does not recheck consent under that mutex. Closing this race remains blocking OPS-100 work.
- The in-memory queue has an event-count limit, but the current local backend has no durable retry, retention, spool-byte, or complete drop/failure accounting contract.

## Built-in Backend: LocalFileTelemetryBackend

The local file backend writes JSON files to a specified directory. Each flush produces a timestamped file.

### Automatic creation

If `TelemetryConfig::localExportPath` is non-empty and no backend has been registered, `Initialize()` automatically creates a `LocalFileTelemetryBackend`:

```cpp
Spark::TelemetryConfig cfg;
cfg.localExportPath = "telemetry/";
telemetry.Initialize(cfg);
// LocalFileTelemetryBackend is now active
```

### Output format

Each flush writes a file like `telemetry/telemetry_1712188800000.json`:

```json
[
  {
    "name": "level_start",
    "timestamp": 1712188800000,
    "sessionId": "session_1712188800000",
    "properties": {
      "level": "3",
      "difficulty": "hard"
    }
  },
  {
    "name": "player_death",
    "timestamp": 1712188830000,
    "sessionId": "session_1712188800000",
    "properties": {
      "cause": "fall_damage"
    }
  }
]
```

### Querying backend stats

```cpp
// If you have a direct reference to the backend:
auto* localBackend = dynamic_cast<Spark::LocalFileTelemetryBackend*>(backend.get());
if (localBackend)
{
    uint32_t filesWritten = localBackend->GetFilesWritten();
}
```

## Writing Custom Backends

Implement `ITelemetryBackend` to send events to any destination:

```cpp
class HttpTelemetryBackend final : public Spark::ITelemetryBackend
{
public:
    explicit HttpTelemetryBackend(std::string endpoint)
        : m_endpoint(std::move(endpoint)) {}

    bool Send(const std::vector<Spark::TelemetryEvent>& events) override
    {
        // Serialize events to JSON
        std::string payload = SerializeToJson(events);

        // POST to the configured endpoint
        auto response = HttpClient::Post(m_endpoint, payload);
        return response.statusCode == 200;
    }

    std::string_view GetBackendName() const override
    {
        return "HTTP";
    }

private:
    std::string m_endpoint;
};
```

### Registering a custom backend

```cpp
auto& telemetry = Spark::TelemetrySystem::GetInstance();

// Register before or after Initialize() -- replaces any existing backend
telemetry.RegisterBackend(
    std::make_unique<HttpTelemetryBackend>("https://analytics.example.com/v1/events")
);
```

Note: `RegisterBackend()` replaces the current backend. Only one backend is active at a time. To fan out to multiple destinations, create a composite backend that delegates to multiple sub-backends.

## Console Commands

| Method | Description |
|---|---|
| `Console_GetStatus()` | Returns initialization state, enabled/consent flags, queue size, total events recorded, backend name, and session ID |

Example output:

```
TelemetrySystem: initialized | Enabled: yes | Consent: yes | Queued: 12/10000 | Total recorded: 347 | Backend: LocalFile | Session: session_1712188800000
```

## Integration

### With the Main Loop

Call `Update(dt)` every frame for auto-flush behavior:

```cpp
void Engine::Tick(float dt)
{
    // ... other systems ...
    telemetry.Update(dt);
}
```

### With the Profiler

Record performance metrics as timed events:

```cpp
auto start = std::chrono::steady_clock::now();
RenderFrame();
auto end = std::chrono::steady_clock::now();
float ms = std::chrono::duration<float, std::milli>(end - start).count();

telemetry.RecordTimedEvent("frame_render", ms, {
    {"draw_calls", std::to_string(drawCallCount)},
    {"triangles", std::to_string(triCount)}
});
```

### With the Settings UI

Wire consent to a user-facing toggle in the settings menu:

```cpp
// In settings panel:
bool consent = telemetry.HasConsent();
if (ImGui::Checkbox("Allow anonymous usage data", &consent))
{
    telemetry.SetConsent(consent);
    SaveUserPreferences();
}
```

### With GamePackager / CI

Record packaging metrics for build analytics:

```cpp
auto start = std::chrono::steady_clock::now();
auto result = packager.Package(cfg);
auto elapsed = std::chrono::duration<float, std::milli>(
    std::chrono::steady_clock::now() - start).count();

telemetry.RecordTimedEvent("game_package", elapsed, {
    {"success", result.success ? "true" : "false"},
    {"assets", std::to_string(result.assetCount)},
    {"size_mb", std::to_string(result.totalSizeMB)}
});
```

### Session Identification

`Initialize()` generates a session ID from the current epoch time (e.g., `session_1712188800000`). All events recorded in this session carry the same ID, enabling per-session grouping in analytics tools.

## API Reference

### TelemetrySystem

| Method | Signature | Description |
|---|---|---|
| `GetInstance` | `static TelemetrySystem& GetInstance()` | Get the singleton instance |
| `Initialize` | `void Initialize(const TelemetryConfig& config)` | Initialize with configuration; auto-creates LocalFile backend if path is set |
| `Shutdown` | `void Shutdown()` | Flush remaining events, clear queue, release backend |
| `RecordEvent` | `void RecordEvent(std::string_view name, const std::optional<std::unordered_map<std::string, std::string>>& properties = std::nullopt)` | Record a telemetry event with optional properties |
| `RecordTimedEvent` | `void RecordTimedEvent(std::string_view name, float durationMs, const std::optional<std::unordered_map<std::string, std::string>>& properties = std::nullopt)` | Record a timed event with duration_ms property |
| `FlushEvents` | `void FlushEvents()` | Flush all queued events to the backend immediately |
| `Update` | `void Update(float dt)` | Per-frame update; auto-flushes when interval elapses |
| `SetConsent` | `void SetConsent(bool consent)` | Set user consent; revoking clears the queue |
| `HasConsent` | `bool HasConsent() const` | Check if user has given consent |
| `GetConfig` | `const TelemetryConfig& GetConfig() const` | Get current configuration (read-only) |
| `GetQueueSize` | `uint32_t GetQueueSize() const` | Number of events currently queued |
| `RegisterBackend` | `void RegisterBackend(std::unique_ptr<ITelemetryBackend> backend)` | Register a custom backend (replaces existing) |
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Human-readable status string |

### ITelemetryBackend

| Method | Signature | Description |
|---|---|---|
| `Send` | `virtual bool Send(const std::vector<TelemetryEvent>& events) = 0` | Send a batch of events; return true if accepted |
| `GetBackendName` | `virtual std::string_view GetBackendName() const = 0` | Human-readable backend name |

### LocalFileTelemetryBackend

| Method | Signature | Description |
|---|---|---|
| Constructor | `explicit LocalFileTelemetryBackend(std::string exportPath)` | Create with output directory path |
| `Send` | `bool Send(const std::vector<TelemetryEvent>& events) override` | Write events to a timestamped JSON file |
| `GetBackendName` | `std::string_view GetBackendName() const override` | Returns `"LocalFile"` |
| `GetFilesWritten` | `uint32_t GetFilesWritten() const` | Number of JSON files successfully written |

## Thread Safety

`RecordEvent()` supports multiple producers through the queue mutex and atomic sequence counter. Configuration, consent, flush, update, backend registration, and shutdown are not a general concurrently callable API; coordinate lifecycle operations on the owning thread. In particular, the consent check and enqueue are separate critical sections, which creates the revocation race above.

The singleton initialization is safe under C++11. `LocalFileTelemetryBackend::Send()` performs file I/O and should not be called concurrently on the same instance.

## See Also

- [Asset-Validation](../gameplay-tools/Asset-Validation.md) -- Record validation metrics as telemetry events
- [Accessibility](../platform/Accessibility.md) -- Track accessibility feature usage
- [Platform-Input](../platform/Platform-Input.md) -- Record input analytics
- [Game-Packaging](../gameplay-tools/Game-Packaging.md) -- Record packaging metrics
