# Replay System

SparkEngine provides a replay recording and playback system for FPS games. It captures entity state snapshots at configurable intervals, supporting full match replays, kill cams, slow-motion playback, and multiple camera modes. The system uses a compact binary file format with safety-bounded deserialization.

**Source:** `SparkEngine/Source/Engine/Replay/ReplaySystem.h`

## Architecture Overview

The replay system records per-frame snapshots of all entity states and discrete gameplay events into a `ReplayData` structure. During playback, it uses binary search to seek to any point in time and provides the corresponding frame data for the game to apply to the scene.

```
                    RECORDING
                    =========

  Game Loop          ReplaySystem            ReplayData
  +---------+        +---------------+        +-----------------+
  | Entity  | -----> | RecordFrame() | -----> | frames[]        |
  | States  |        |               |        |   [0] t=0.00s   |
  +---------+        | RecordEvent() | -----> |   [1] t=0.05s   |
  | Events  | -----> |               |        |   [2] t=0.10s   |
  +---------+        +---------------+        |   ...           |
                                              | events[]        |
                                              |   kill @ 12.5s  |
                                              |   pickup @ 30s  |
                                              +-----------------+
                                                      |
                                               SaveToFile()
                                                      |
                                                      v
                                              +------------------+
                                              | match_001.replay |
                                              | (binary file)    |
                                              +------------------+

                    PLAYBACK
                    ========

  +------------------+        +------------------+        +---------+
  | match_001.replay | -----> | LoadFromFile()   |        | Scene   |
  +------------------+        | StartPlayback()  |        |         |
                              | UpdatePlayback() | -----> | Apply   |
                              | GetCurrentFrame()| -----> | Entity  |
                              | SeekTo()         |        | States  |
                              +------------------+        +---------+
```

| Class | Responsibility |
|-------|---------------|
| `ReplaySystem` | Records frames, manages playback, kill cam, file I/O |
| `ReplayData` | Complete recording: frames + events + metadata |
| `ReplayFrame` | Snapshot of all entity states at one point in time |
| `ReplayEntityState` | Position, rotation, velocity, health, flags for one entity |
| `ReplayEvent` | Discrete gameplay event (kill, explosion, pickup) |
| `PlaybackCamera` | Enum for camera modes during playback |
| `PlaybackState` | Enum for current playback state |

## Namespace and Header

```cpp
#include "Engine/Replay/ReplaySystem.h"

// All types live in the Spark namespace
using namespace Spark;
```

Required headers pulled in by `ReplaySystem.h`:

| Header | Purpose |
|--------|---------|
| `Core/Platform.h` | Cross-platform math type stubs |
| `<DirectXMath.h>` | `XMFLOAT3`, `XMFLOAT4` for positions and rotations |
| `<string>` | Map name, game mode, event types, file paths |
| `<vector>` | Frame and event storage |
| `<unordered_map>` | (included but not currently used in public API) |
| `<functional>` | (included for future callback support) |
| `<cstdint>` | `uint32_t` for entity IDs, frame numbers |
| `<mutex>` | Thread-safe recording and playback access |

## Full API Reference

### ReplayEntityState

Snapshot of a single entity's state at a point in time.

```cpp
struct ReplayEntityState
{
    uint32_t entityId = 0;                       ///< Unique entity identifier
    DirectX::XMFLOAT3 position{0, 0, 0};        ///< World-space position
    DirectX::XMFLOAT4 rotation{0, 0, 0, 1};     ///< Orientation as quaternion (x, y, z, w)
    DirectX::XMFLOAT3 velocity{0, 0, 0};        ///< Linear velocity
    float health = 100.0f;                       ///< Current health points
    int animationState = 0;                      ///< Index into animation state machine
    uint32_t flags = 0;                          ///< Bitmask: alive, visible, firing, etc.
};
```

**Field Details:**

| Field | Type | Size | Description |
|-------|------|------|-------------|
| `entityId` | `uint32_t` | 4 bytes | Unique identifier matching the ECS entity. Used to map replay state back to scene entities during playback. |
| `position` | `XMFLOAT3` | 12 bytes | World-space position of the entity. For characters, this is typically the root position. |
| `rotation` | `XMFLOAT4` | 16 bytes | Orientation as a quaternion `(x, y, z, w)`. Identity rotation is `(0, 0, 0, 1)`. |
| `velocity` | `XMFLOAT3` | 12 bytes | Linear velocity in world-space units per second. Used for interpolation during playback. |
| `health` | `float` | 4 bytes | Health points at the time of the snapshot. |
| `animationState` | `int` | 4 bytes | Index into the entity's animation state machine. Allows replay to show correct animations. |
| `flags` | `uint32_t` | 4 bytes | Bitmask encoding boolean states. |

**Entity Flags Bitmask:**

| Bit | Mask | Meaning |
|-----|------|---------|
| 0 | `0x01` | Alive |
| 1 | `0x02` | Visible |
| 2 | `0x04` | Firing weapon |
| 3 | `0x08` | Crouching |
| 4 | `0x10` | Sprinting |
| 5 | `0x20` | Reloading |
| 6 | `0x40` | In vehicle |
| 7 | `0x80` | Scoped / aiming down sights |

### ReplayFrame

A single frame of replay data containing the state of all entities at one timestamp.

```cpp
struct ReplayFrame
{
    float timestamp = 0.0f;                  ///< Game time in seconds
    uint32_t frameNumber = 0;                ///< Sequential frame number
    std::vector<ReplayEntityState> entities; ///< All entity states this frame
};
```

Frames are stored in chronological order by `timestamp`. The `frameNumber` is a monotonically increasing counter set during recording. The `entities` vector contains the state of every tracked entity at this point in time.

### ReplayEvent

A discrete event recorded during gameplay, separate from the continuous state snapshots.

```cpp
struct ReplayEvent
{
    float timestamp = 0.0f;                    ///< Game time when the event occurred
    std::string type;                          ///< Event type identifier
    uint32_t sourceEntity = 0;                 ///< Entity that caused the event
    uint32_t targetEntity = 0;                 ///< Entity affected by the event
    DirectX::XMFLOAT3 position{0, 0, 0};      ///< World-space location of the event
    std::string data;                          ///< Additional event-specific data (JSON or key=value)
};
```

**Standard Event Types:**

| Type String | Source Entity | Target Entity | Position | Data Field |
|-------------|--------------|---------------|----------|------------|
| `"kill"` | Killer | Victim | Kill location | Weapon name |
| `"explosion"` | Grenade owner | 0 (area) | Explosion center | Radius, damage |
| `"pickup"` | Player | 0 | Pickup location | Item name |
| `"spawn"` | Player | 0 | Spawn point | Team name |
| `"flag_capture"` | Player | 0 | Flag location | Team name |
| `"objective"` | Player | 0 | Objective location | Objective ID |
| `"weapon_switch"` | Player | 0 | Player position | New weapon name |

### ReplayData

Complete replay recording containing all frames, events, and metadata.

```cpp
struct ReplayData
{
    std::string mapName;             ///< Map the replay was recorded on
    std::string gameMode;            ///< Game mode name (e.g. "deathmatch", "ctf")
    float duration = 0.0f;           ///< Total recording duration in seconds
    uint32_t version = 1;            ///< Replay format version
    std::vector<ReplayFrame> frames; ///< All recorded frames (chronological order)
    std::vector<ReplayEvent> events; ///< All recorded events (chronological order)
};
```

### PlaybackCamera

Camera modes available during replay playback.

```cpp
enum class PlaybackCamera
{
    FreeCam,     ///< Free-flying camera controlled by the viewer
    FollowCam,   ///< Third-person camera following an entity
    FirstPerson, ///< First-person view from an entity's perspective
    KillCam      ///< Cinematic kill camera with slow-motion
};
```

| Mode | Description | Use Case |
|------|-------------|----------|
| `FreeCam` | Mouse/keyboard-controlled free-flying camera. No entity attachment. | Map overview, spectating, analysis |
| `FollowCam` | Third-person camera that orbits the followed entity. | Watching a specific player |
| `FirstPerson` | Camera placed at the entity's head position and orientation. | Experiencing the game from a player's POV |
| `KillCam` | Automated cinematic camera that follows the kill event. Slow-motion by default. | Post-death replay |

### PlaybackState

Current state of replay playback.

```cpp
enum class PlaybackState
{
    Stopped,     ///< Not playing; playback time is at 0
    Playing,     ///< Advancing forward through the replay
    Paused,      ///< Frozen at the current time
    Rewinding,   ///< Playing backward
    FastForward  ///< Playing forward at accelerated speed
};
```

**Playback State Machine:**

```
                StartPlayback()
  Stopped -----------------------> Playing
     ^                               |
     |          StopPlayback()       |
     +-------------------------------+
     |                               |
     |          PausePlayback()      |
     |            +-------> Paused   |
     |            |           |      |
     |            +-----------+      |
     |          ResumePlayback()     |
     |                               |
     +--- (playback reaches end) ----+
```

### ReplaySystem Class

```cpp
class ReplaySystem
{
public:
    ReplaySystem();
    ~ReplaySystem() = default;

    // --- Recording ---
    void SetRecordInterval(float seconds);
    void StartRecording();
    void StopRecording();
    bool IsRecording() const;
    void RecordFrame(const std::vector<ReplayEntityState>& entities, float timestamp);
    void RecordEvent(const ReplayEvent& event);
    void SetMetadata(const std::string& mapName, const std::string& gameMode);

    // --- Playback ---
    void StartPlayback();
    void PausePlayback();
    void ResumePlayback();
    void StopPlayback();
    void UpdatePlayback(float deltaTime);
    void SeekTo(float timestamp);
    void SetPlaybackSpeed(float speed);
    float GetPlaybackSpeed() const;
    PlaybackState GetPlaybackState() const;
    float GetPlaybackTime() const;
    float GetDuration() const;
    const ReplayFrame* GetCurrentFrame() const;
    std::vector<ReplayEvent> GetEventsNearTime(float windowSeconds = 0.1f) const;

    // --- Kill Cam ---
    void StartKillCam(float rewindSeconds, uint32_t focusEntity);
    void StopKillCam();
    bool IsKillCamActive() const;

    // --- Camera ---
    void SetCamera(PlaybackCamera mode);
    PlaybackCamera GetCamera() const;
    void SetFollowEntity(uint32_t entityId);

    // --- File I/O ---
    bool SaveToFile(const std::string& filePath) const;
    bool LoadFromFile(const std::string& filePath);

    // --- Console ---
    std::string Console_GetStatus() const;
};
```

**Method Reference:**

| Method | Return | Description |
|--------|--------|-------------|
| `SetRecordInterval(sec)` | `void` | Sets the minimum time between recorded frames. Default: `0.05` (20 fps). Lower values increase file size but capture more detail. |
| `StartRecording()` | `void` | Begins recording. Clears any existing replay data. |
| `StopRecording()` | `void` | Stops recording and finalizes the duration. |
| `IsRecording()` | `bool` | Returns `true` if recording is active. |
| `RecordFrame(entities, time)` | `void` | Records a snapshot of all entity states. Only records if the recording interval has elapsed. |
| `RecordEvent(event)` | `void` | Records a discrete gameplay event. Events are stored separately from frames. |
| `SetMetadata(map, mode)` | `void` | Sets the map name and game mode for the replay file header. |
| `StartPlayback()` | `void` | Begins playback from the start (time = 0). |
| `PausePlayback()` | `void` | Pauses playback at the current time. |
| `ResumePlayback()` | `void` | Resumes playback from a paused state. |
| `StopPlayback()` | `void` | Stops playback and resets time to 0. |
| `UpdatePlayback(dt)` | `void` | Advances playback time by `dt * playbackSpeed`. Finds the corresponding frame via binary search. Stops automatically when reaching the end. |
| `SeekTo(timestamp)` | `void` | Jumps to the specified time. Clamps to `[0, duration]`. |
| `SetPlaybackSpeed(speed)` | `void` | Sets the playback speed multiplier. 1.0 = normal, 0.25 = quarter, 2.0 = double. Negative values are not supported (use Rewinding state). |
| `GetPlaybackSpeed()` | `float` | Returns current playback speed. |
| `GetPlaybackState()` | `PlaybackState` | Returns the current playback state enum. |
| `GetPlaybackTime()` | `float` | Returns the current playback time in seconds. |
| `GetDuration()` | `float` | Returns the total duration of the loaded/recorded replay. |
| `GetCurrentFrame()` | `const ReplayFrame*` | Returns a pointer to the frame nearest to the current playback time. Returns `nullptr` if no frames are loaded. |
| `GetEventsNearTime(window)` | `vector<ReplayEvent>` | Returns all events within `[currentTime - window, currentTime + window]`. Default window is 0.1 seconds. |
| `StartKillCam(rewind, entity)` | `void` | Starts kill cam: seeks to `duration - rewindSeconds`, sets speed to 0.5x, follows the specified entity with `KillCam` camera mode. |
| `StopKillCam()` | `void` | Stops kill cam, resets speed to 1.0x, stops playback. |
| `IsKillCamActive()` | `bool` | Returns `true` if kill cam is currently active. |
| `SetCamera(mode)` | `void` | Sets the camera mode for playback. |
| `GetCamera()` | `PlaybackCamera` | Returns the current camera mode. |
| `SetFollowEntity(id)` | `void` | Sets which entity the FollowCam and FirstPerson cameras track. |
| `SaveToFile(path)` | `bool` | Writes the replay to a binary file. Returns `false` if the file cannot be opened. |
| `LoadFromFile(path)` | `bool` | Reads a replay from a binary file. Returns `false` on I/O errors, bad magic, or bounds violations. |
| `Console_GetStatus()` | `std::string` | Returns a formatted status string showing recording state, playback state, time, speed, frame/event counts, map, mode, and kill cam status. |

## Quick Start

### Recording

```cpp
ReplaySystem replay;
replay.SetRecordInterval(1.0f / 20.0f);  // 20 snapshots per second
replay.SetMetadata("de_dust2", "deathmatch");
replay.StartRecording();

// In your game loop:
void GameLoop::Update(float deltaTime)
{
    // Gather entity states from ECS
    std::vector<ReplayEntityState> states;
    auto view = registry.view<TransformComponent, HealthComponent>();
    for (auto [entity, transform, health] : view.each())
    {
        ReplayEntityState state;
        state.entityId = static_cast<uint32_t>(entity);
        state.position = transform.position;
        state.rotation = transform.rotation;
        state.velocity = transform.velocity;
        state.health = health.current;
        state.flags = health.IsAlive() ? 0x01 : 0x00;
        states.push_back(state);
    }

    replay.RecordFrame(states, currentGameTime);
}

// Record events when they happen:
void OnKill(uint32_t killer, uint32_t victim, const XMFLOAT3& pos)
{
    ReplayEvent event;
    event.timestamp = currentGameTime;
    event.type = "kill";
    event.sourceEntity = killer;
    event.targetEntity = victim;
    event.position = pos;
    event.data = "weapon=ak47";
    replay.RecordEvent(event);
}

// When the match ends:
replay.StopRecording();
replay.SaveToFile("replays/match_001.replay");
```

### Playback

```cpp
ReplaySystem replay;
if (!replay.LoadFromFile("replays/match_001.replay"))
{
    LogError("Failed to load replay file");
    return;
}

replay.SetCamera(PlaybackCamera::FreeCam);
replay.StartPlayback();

// In your replay viewer loop:
void ReplayViewer::Update(float deltaTime)
{
    replay.UpdatePlayback(deltaTime);

    const ReplayFrame* frame = replay.GetCurrentFrame();
    if (frame)
    {
        // Apply entity states to the scene
        for (const auto& entityState : frame->entities)
        {
            ApplyEntityState(entityState);
        }

        // Check for events near this time
        auto events = replay.GetEventsNearTime(0.1f);
        for (const auto& event : events)
        {
            if (event.type == "kill")
            {
                ShowKillFeedEntry(event);
            }
            else if (event.type == "explosion")
            {
                SpawnExplosionEffect(event.position);
            }
        }
    }

    // Display HUD
    float time = replay.GetPlaybackTime();
    float duration = replay.GetDuration();
    DrawTimelineBar(time, duration);
}
```

### Playback Controls

```cpp
// Speed control
replay.SetPlaybackSpeed(0.25f);  // Quarter speed (slow motion)
replay.SetPlaybackSpeed(1.0f);   // Normal speed
replay.SetPlaybackSpeed(2.0f);   // Double speed
replay.SetPlaybackSpeed(4.0f);   // Fast forward

// Seeking
replay.SeekTo(30.0f);   // Jump to 30 seconds
replay.SeekTo(0.0f);    // Jump to start

// Pause / resume
replay.PausePlayback();
replay.ResumePlayback();

// Camera modes
replay.SetCamera(PlaybackCamera::FreeCam);
replay.SetCamera(PlaybackCamera::FollowCam);
replay.SetFollowEntity(playerEntityId);
replay.SetCamera(PlaybackCamera::FirstPerson);
```

## Kill Cam

The kill cam system rewinds the replay to show the moments before a player's death, typically from the victim's perspective with slow-motion.

```cpp
// When a player dies, start kill cam:
// Rewind 5 seconds and focus on the victim
replay.StartKillCam(5.0f, victimEntity);

// The kill cam automatically:
// 1. Seeks to (duration - rewindSeconds)
// 2. Sets playback speed to 0.5x (slow motion)
// 3. Sets camera to KillCam mode
// 4. Sets follow entity to the focus entity
// 5. Starts playback

// Stop kill cam and return to gameplay
replay.StopKillCam();
// This resets speed to 1.0x and stops playback
```

### Kill Cam Integration Pattern

```cpp
void OnPlayerDeath(uint32_t killer, uint32_t victim)
{
    // Record the kill event
    ReplayEvent killEvent;
    killEvent.timestamp = currentTime;
    killEvent.type = "kill";
    killEvent.sourceEntity = killer;
    killEvent.targetEntity = victim;
    replay.RecordEvent(killEvent);

    // Show kill cam if this is the local player
    if (victim == localPlayerEntity)
    {
        replay.StartKillCam(5.0f, victim);

        // In the kill cam update loop:
        // UpdatePlayback() advances through the recorded frames
        // When kill cam playback reaches the end (the death moment),
        // call StopKillCam() and return to the respawn screen
    }
}
```

## Binary File Format

The replay file uses a compact binary format with the magic number `RPLY` (0x52504C59).

```
+----------------------------------+
| Header                           |
|   magic: uint32 = 0x52504C59    |  4 bytes
|   version: uint32               |  4 bytes
+----------------------------------+
| Metadata                         |
|   mapName length: uint32        |  4 bytes
|   mapName: char[]               |  variable
|   gameMode length: uint32       |  4 bytes
|   gameMode: char[]              |  variable
|   duration: float               |  4 bytes
+----------------------------------+
| Frames                           |
|   frameCount: uint32            |  4 bytes
|   for each frame:               |
|     timestamp: float            |  4 bytes
|     frameNumber: uint32         |  4 bytes
|     entityCount: uint32         |  4 bytes
|     for each entity:            |
|       ReplayEntityState         |  56 bytes (struct)
+----------------------------------+
| Events                           |
|   eventCount: uint32            |  4 bytes
|   for each event:               |
|     timestamp: float            |  4 bytes
|     type length: uint32         |  4 bytes
|     type: char[]                |  variable
|     sourceEntity: uint32        |  4 bytes
|     targetEntity: uint32        |  4 bytes
|     position: XMFLOAT3          |  12 bytes
+----------------------------------+
```

### File Size Estimates

| Scenario | Duration | Entities | Record Rate | Approx Size |
|----------|----------|----------|-------------|-------------|
| Kill cam (short) | 5 sec | 20 | 20 fps | ~112 KB |
| Single round | 3 min | 20 | 20 fps | ~4 MB |
| Full match | 30 min | 20 | 20 fps | ~40 MB |
| Full match | 30 min | 20 | 10 fps | ~20 MB |
| Full match (64 players) | 30 min | 64 | 20 fps | ~130 MB |

**Formula:** `size ~= frameCount * entityCount * 56 bytes + eventOverhead`

Where `frameCount = duration * recordRate`.

### Deserialization Safety Limits

The `LoadFromFile()` implementation enforces strict bounds to prevent out-of-memory attacks from malicious or corrupted replay files:

| Limit | Value | Purpose |
|-------|-------|---------|
| `kMaxStringLength` | 4,096 | Maximum characters for map name, game mode, event type |
| `kMaxFrameCount` | 1,000,000 | Maximum number of frames (~13.9 hours at 20 fps) |
| `kMaxEntityCount` | 100,000 | Maximum entities per frame |
| `kMaxEventCount` | 1,000,000 | Maximum discrete events |

If any of these limits are exceeded during deserialization, `LoadFromFile()` returns `false` immediately without allocating further memory. Every `file.read()` call is followed by a stream validity check (`if (!file)`) to detect truncated files.

## Frame Seeking Algorithm

The `SeekTo()` and `UpdatePlayback()` methods use binary search (`std::lower_bound`) to find the frame nearest to the requested timestamp.

```
Frames:  [0.00] [0.05] [0.10] [0.15] [0.20] [0.25] [0.30]

SeekTo(0.12):
    lower_bound finds frame at index 3 (timestamp 0.15)
    Returns frame[3]

SeekTo(0.20):
    lower_bound finds frame at index 4 (timestamp 0.20)
    Returns frame[4] (exact match)

SeekTo(99.0):  (beyond end)
    lower_bound reaches end
    Returns last frame
```

This provides O(log N) seeking, where N is the number of frames.

## Thread Safety

`ReplaySystem` is protected by a `std::mutex` (`m_mutex`) for concurrent access. Both recording and playback methods acquire the lock.

| Operation | Thread Safe | Notes |
|-----------|------------|-------|
| `StartRecording()` | Yes | Acquires mutex; clears replay data |
| `StopRecording()` | Yes | Acquires mutex; sets duration |
| `RecordFrame()` | Yes | Acquires mutex; appends frame |
| `RecordEvent()` | Yes | Acquires mutex; appends event |
| `SetMetadata()` | Yes | Acquires mutex |
| `StartPlayback()` | Yes | Acquires mutex; resets playback state |
| `PausePlayback()` | Yes | Acquires mutex |
| `ResumePlayback()` | Yes | Acquires mutex |
| `StopPlayback()` | Yes | Acquires mutex |
| `UpdatePlayback()` | Yes | Acquires mutex; advances time |
| `SeekTo()` | Yes | Acquires mutex; binary search |
| `GetCurrentFrame()` | Yes | Acquires mutex; returns pointer to internal data |
| `GetEventsNearTime()` | Yes | Acquires mutex; returns copy of matching events |
| `SaveToFile()` | Yes | Acquires mutex; blocks on file I/O |
| `LoadFromFile()` | Yes | Acquires mutex; blocks on file I/O |
| `StartKillCam()` | Partial | Does not acquire mutex before calling `SeekTo()` (which acquires its own lock). The kill cam state members are written without the lock. |
| `StopKillCam()` | No | Writes kill cam state without acquiring mutex |

**Note:** `StartKillCam()` and `StopKillCam()` have incomplete thread safety. If these methods may be called from threads other than the main thread, external synchronization is required.

**Important:** The pointer returned by `GetCurrentFrame()` points to internal data protected by the mutex. The mutex is released when `GetCurrentFrame()` returns, so the pointer may become invalid if another thread modifies the replay data. Copy the frame data if it will be used across threads.

## Integration with EngineContext

Register the replay system with the engine service locator:

```cpp
// During engine initialization
auto replay = std::make_unique<ReplaySystem>();
replay->SetRecordInterval(1.0f / 20.0f);
EngineContext::Register<ReplaySystem>(std::move(replay));

// From any system:
auto* replay = EngineContext::Get<ReplaySystem>();
if (replay && replay->IsRecording())
{
    replay->RecordFrame(entityStates, currentTime);
}
```

## Integration with ECS

```cpp
// Recording system that runs each frame after all other systems
class ReplayRecordSystem
{
public:
    void Update(float currentTime, entt::registry& registry, ReplaySystem& replay)
    {
        if (!replay.IsRecording())
        {
            return;
        }

        std::vector<ReplayEntityState> states;
        auto view = registry.view<TransformComponent, HealthComponent, ReplayTagComponent>();
        states.reserve(view.size_hint());

        for (auto [entity, transform, health, tag] : view.each())
        {
            ReplayEntityState state;
            state.entityId = static_cast<uint32_t>(entity);
            state.position = transform.position;
            state.rotation = transform.rotation;
            state.velocity = transform.velocity;
            state.health = health.current;
            state.animationState = tag.lastAnimState;
            state.flags = BuildFlags(health, transform);
            states.push_back(state);
        }

        replay.RecordFrame(states, currentTime);
    }

private:
    uint32_t BuildFlags(const HealthComponent& h, const TransformComponent& t)
    {
        uint32_t flags = 0;
        if (h.IsAlive()) flags |= 0x01;
        if (t.isVisible) flags |= 0x02;
        return flags;
    }
};
```

## Interpolation Between Frames

For smooth playback, especially at low recording rates, interpolate between adjacent frames:

```cpp
void InterpolateFrame(const ReplayFrame& frameA, const ReplayFrame& frameB,
                      float t, std::vector<ReplayEntityState>& result)
{
    // t is in [0, 1] between frameA and frameB timestamps
    for (size_t i = 0; i < frameA.entities.size() && i < frameB.entities.size(); ++i)
    {
        const auto& a = frameA.entities[i];
        const auto& b = frameB.entities[i];

        if (a.entityId != b.entityId)
        {
            continue;  // Entity mismatch; skip interpolation
        }

        ReplayEntityState interp;
        interp.entityId = a.entityId;

        // Lerp position
        interp.position.x = a.position.x + (b.position.x - a.position.x) * t;
        interp.position.y = a.position.y + (b.position.y - a.position.y) * t;
        interp.position.z = a.position.z + (b.position.z - a.position.z) * t;

        // Slerp rotation (quaternion)
        // interp.rotation = QuaternionSlerp(a.rotation, b.rotation, t);

        // Use latest discrete state
        interp.health = (t < 0.5f) ? a.health : b.health;
        interp.flags = (t < 0.5f) ? a.flags : b.flags;
        interp.animationState = (t < 0.5f) ? a.animationState : b.animationState;

        result.push_back(interp);
    }
}
```

## Recording Rate Guidelines

| Use Case | Record Rate | Interval | Quality | File Size |
|----------|-----------|----------|---------|-----------|
| Kill cam | 30 fps | 0.033s | High (smooth slow-mo) | Large |
| Full match replay | 20 fps | 0.05s | Good (default) | Medium |
| Long session recording | 10 fps | 0.1s | Acceptable | Small |
| Debug / analytics | 5 fps | 0.2s | Low (positions only) | Very small |

Higher recording rates produce smoother playback, especially during slow-motion kill cams. Lower rates reduce file size for long recording sessions.

## Console Commands

```
replay_status           # Show recording/playback status
replay_start            # Start recording
replay_stop             # Stop recording
replay_save <path>      # Save replay to file
replay_load <path>      # Load replay from file
replay_play             # Start playback
replay_pause            # Pause playback
replay_seek <seconds>   # Seek to time
replay_speed <mult>     # Set playback speed multiplier
replay_camera <mode>    # Set camera: free, follow, firstperson, killcam
replay_follow <entity>  # Set follow entity ID
replay_killcam <secs>   # Start kill cam with rewind duration
```

### Console Output Example

```
=== Replay System ===
Recording: NO
Playback: Playing
Playback time: 45.3s / 180.0s
Speed: 1x
Frames recorded: 3600
Events recorded: 47
Map: de_dust2
Mode: deathmatch
Kill cam: OFF
```

## Performance Considerations

| Concern | Recommendation |
|---------|---------------|
| Memory usage during long matches | Use lower record rate (10 fps); or periodically flush older frames to disk |
| `RecordFrame()` allocation overhead | Reserve the entity state vector to expected entity count |
| File save blocks the main thread | Move `SaveToFile()` to a background thread |
| Seek latency with millions of frames | Binary search is O(log N); acceptable for up to ~1M frames |
| Playback with many entities | Apply states only to visible entities; skip off-screen updates |

### Memory Usage Estimates

| Component | Per Frame (20 entities) | Per Minute (20 fps) | Per Hour |
|-----------|------------------------|--------------------|---------|
| ReplayFrame (20 entities) | ~1.1 KB | ~1.3 MB | ~79 MB |
| ReplayEvent (avg 2/sec) | ~64 bytes | ~7.5 KB | ~450 KB |

## Error Handling

```cpp
// File I/O errors
if (!replay.SaveToFile("replays/match.replay"))
{
    LogError("Failed to save replay: check disk space and permissions");
}

if (!replay.LoadFromFile("replays/match.replay"))
{
    // Possible causes:
    // - File does not exist
    // - Bad magic number (not a valid replay file)
    // - Version mismatch
    // - Corrupted data (string length or count exceeds safety limits)
    // - Truncated file (EOF reached mid-read)
    LogError("Failed to load replay file");
}

// Null frame during playback
const ReplayFrame* frame = replay.GetCurrentFrame();
if (!frame)
{
    // No frames loaded, or current index is out of range
    LogWarning("No replay frame available at current time");
}
```

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| `SaveToFile()` returns `false` | Disk full or invalid path | Check disk space; ensure directory exists |
| `LoadFromFile()` returns `false` | Corrupted file or wrong format version | Verify file was saved by a compatible version; check file is not truncated |
| Playback appears jerky | Recording rate too low | Increase record rate; implement frame interpolation |
| Kill cam does not show the kill | Rewind duration too short | Increase the `rewindSeconds` parameter |
| Entities teleport during playback | Entity IDs changed between recording and playback | Ensure entity IDs are stable across recording and playback sessions |
| `GetCurrentFrame()` returns `nullptr` | No replay data loaded | Call `LoadFromFile()` or `StartRecording()` + `RecordFrame()` first |
| Memory usage grows unbounded | Recording for hours without stopping | Flush frames to disk periodically; use a circular buffer for kill cam |
| Events missing during playback | `GetEventsNearTime()` window too small | Increase the `windowSeconds` parameter |
| Kill cam starts at wrong time | Recording was still in progress when kill cam started | The kill cam seeks relative to `duration`, which is updated on each `RecordFrame()` call |

---

## See Also

- [Entity Component System](Entity-Component-System) -- Entity states captured in replays
- [Cinematic Sequencer](Cinematic-Sequencer) -- Cinematic camera during kill cams
- [Networking](Networking) -- Multiplayer replay recording
- [Save System](Save-System) -- Persistent data serialization patterns
