# Cinematic Sequencer

SparkEngine includes a timeline-based cinematic sequencer for creating in-game cutscenes and scripted events.

**Source:** `SparkEngine/Source/Engine/Cinematic/Sequencer.h`

`ENABLE_CINEMATIC=ON`

## Overview

The sequencer provides a non-linear timeline editor for:
- Camera movements and cuts
- Entity animations and transformations
- [Audio](Audio) cue playback
- [Event triggers](Event-System)
- UI transitions

## Concepts

### Sequence

A sequence is a collection of tracks that play together over a defined time range.

### Tracks

Each track controls a specific aspect of the scene:

| Track Type | Controls |
|-----------|----------|
| Camera | Camera position, rotation, FOV over time |
| Transform | Entity position, rotation, scale over time |
| Animation | Animation clip playback and blending |
| Audio | Sound effect and music cue timing |
| Event | Custom event triggers at specific times |

### Keyframes

Keyframes define values at specific points in time. The sequencer interpolates between keyframes:
- **Linear** interpolation for smooth transitions
- **Bezier** curves for eased motion
- **Step** interpolation for instant changes

## Playback Control

```cpp
Sequencer sequencer;

// Load a sequence
sequencer.Load("Assets/Cinematics/Intro.seq");

// Playback controls
sequencer.Play();
sequencer.Pause();
sequencer.Stop();
sequencer.SetTime(5.0f);     // Seek to 5 seconds
sequencer.SetSpeed(0.5f);    // Half speed playback

// Check state
bool playing = sequencer.IsPlaying();
float currentTime = sequencer.GetCurrentTime();
float duration = sequencer.GetDuration();
```

## Camera Cuts

The sequencer can smoothly transition between camera angles or make hard cuts:

- **Smooth transition** — Interpolate position and rotation over a duration
- **Hard cut** — Instant switch to a new camera angle
- **Dolly/crane shots** — Move along spline paths

## Integration

Cinematics can be triggered from:
- Game module code (C++)
- [AngelScript scripts](Scripting-with-AngelScript)
- Trigger volumes in [scenes](Scene-Management)
- [Event System](Event-System) callbacks

## Editor Support

The SparkEditor includes a visual timeline editor for creating and editing cinematic sequences. See [SparkEditor](SparkEditor) for details.

---

## See Also

- [Animation](Animation) — Animation tracks in sequences
- [Audio](Audio) — Audio cue playback
- [SparkEditor](SparkEditor) — Timeline editor UI
- [Event System](Event-System) — Event triggers from sequences
- [Scripting with AngelScript](Scripting-with-AngelScript) — Script-driven cinematics
- [Scene Management](Scene-Management) — Scene-based cinematic triggers
