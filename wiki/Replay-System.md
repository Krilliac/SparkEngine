# Replay System

SparkEngine provides a replay recording and playback system for FPS games. It captures entity state snapshots at configurable intervals, supporting full match replays, kill cams, and slow-motion playback.

**Source:** `SparkEngine/Source/Engine/Replay/ReplaySystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `ReplaySystem` | Records frames, manages playback, kill cam, file I/O |
| `ReplayData` | Complete recording: frames + events + metadata |
| `ReplayFrame` | Snapshot of all entity states at one point in time |
| `ReplayEntityState` | Position, rotation, velocity, health, flags for one entity |
| `ReplayEvent` | Discrete gameplay event (kill, explosion, pickup) |

## Quick Start

```cpp
ReplaySystem replay;
replay.SetRecordInterval(1.0f / 20.0f);  // 20 snapshots/second
replay.SetMetadata("de_dust2", "deathmatch");
replay.StartRecording();

// Each frame:
replay.RecordFrame(entityStates, currentTime);
replay.RecordEvent({"kill", shooterEntity, victimEntity, pos, ""});

// Save:
replay.StopRecording();
replay.SaveToFile("replays/match_001.replay");
```

## Playback

```cpp
replay.LoadFromFile("replays/match_001.replay");
replay.StartPlayback();

// Per frame:
replay.UpdatePlayback(deltaTime);
const ReplayFrame* frame = replay.GetCurrentFrame();
// Apply frame->entities to the scene

// Controls
replay.SetPlaybackSpeed(0.25f);  // Quarter speed
replay.SeekTo(30.0f);            // Jump to 30 seconds
replay.PausePlayback();
replay.ResumePlayback();
```

## Camera Modes

```cpp
enum class PlaybackCamera {
    FreeCam,     // Free-flying camera
    FollowCam,   // Third-person follow
    FirstPerson, // First-person view of an entity
    KillCam      // Cinematic kill camera
};

replay.SetCamera(PlaybackCamera::FollowCam);
replay.SetFollowEntity(playerEntity);
```

## Kill Cam

```cpp
// Rewind 5 seconds and focus on the victim
replay.StartKillCam(5.0f, victimEntity);

// Stop kill cam and return to gameplay
replay.StopKillCam();
```

## Playback States

```cpp
enum class PlaybackState {
    Stopped, Playing, Paused, Rewinding, FastForward
};
```

## Thread Safety

`ReplaySystem` is protected by a mutex for concurrent recording access.

## Console Commands

```
replay_status    # Show recording/playback status
```

---

## See Also

- [Entity Component System](Entity-Component-System) — Entity states captured in replays
- [Cinematic Sequencer](Cinematic-Sequencer) — Cinematic camera during kill cams
- [Networking](Networking) — Multiplayer replay recording
