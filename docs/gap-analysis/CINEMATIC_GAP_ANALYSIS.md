# SparkEngine Cinematic/Sequencer — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Cinematic/` (Sequencer, SequencerManager)
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Cinematic/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Cinematic subsystem provides a timeline-based sequencer for cutscenes. It supports camera path animation (Catmull-Rom splines), entity transform/property keyframes, audio cues, event callbacks, subtitles, and screen fades. The architecture is clean with a base `SequencerTrack` class and typed track subclasses. `SequencerManager` is a singleton that manages named sequences and provides console integration. However, the system lacks serialization, editor integration, and has platform-specific type dependencies throughout its data structures.

---

## Major Gaps

### GAP-CIN01 — No Sequence Serialization (Save/Load)

**Files**:
- `Cinematic/Sequencer.h` (full file — no serialization methods)

**Impact**: Sequences can only be created programmatically via the builder API. There is no way to save a sequence to disk or load one from a file. This means cutscenes must be hardcoded in C++, making them impossible to author or iterate on without recompilation. For a game engine targeting FPS games, this makes cutscene content creation impractical.

**Evidence**: `Sequence` class has no `Save()`, `Load()`, `Serialize()`, or `Deserialize()` methods. `SequencerManager` has no file I/O methods. No JSON, XML, or binary format is defined for sequences.

**What is needed**: Define a JSON or binary format for sequences. Add `Sequence::SaveToFile(path)` and `Sequence::LoadFromFile(path)` methods. Register with the `ComponentSerializerRegistry` if sequences are tied to entities.

---

### GAP-CIN02 — No Editor Integration for Timeline Authoring

**Files**:
- `Cinematic/Sequencer.h`
- `SparkEditor/Source/` (no Cinematic or Sequencer editor panel)

**Impact**: The SparkEditor has 22 subsystem directories but no Cinematic/Sequencer panel. Cutscene authoring requires a visual timeline editor to place keyframes, preview camera paths, and synchronize audio/subtitle cues. Without this, the sequencer is only usable by engine programmers.

**Evidence**: `SparkEditor/Source/` contains `Animation/`, `Lighting/`, `MaterialEditor/`, `Terrain/`, `VisualScripting/`, etc., but no `Cinematic/` or `Sequencer/` directory. The `Sequencer.h` API returns raw pointers to tracks that would need to be visualized in a timeline UI.

**What is needed**: Create an ImGui-based timeline editor panel in the editor. Show tracks as horizontal lanes, keyframes as draggable points, and provide real-time preview with play/pause/scrub controls.

---

### GAP-CIN03 — Platform-Dependent Data Structures

**Files**:
- `Cinematic/Sequencer.h` (lines 47–51, 62–66, 89, 105, 113)

**Impact**: `CameraKeyframe`, `VectorKeyframe`, `AudioCue`, `SubtitleCue`, and `FadeKeyframe` all use DirectXMath types (`XMFLOAT3`, `XMFLOAT4`) directly in their member definitions. These types are only available on Windows behind `#ifdef SPARK_PLATFORM_WINDOWS`, so the entire cinematic system fails to compile on Linux/macOS.

**Evidence**: `XMFLOAT3 position` at line 48, `XMFLOAT3 lookAt` at line 49, `XMFLOAT4 color` at line 105, `XMFLOAT3 color` at line 113 — all without platform guards.

**What is needed**: Use the engine's `Platform.h` cross-platform math stubs, or define platform-agnostic `Vector3`/`Vector4` types. Replace all DirectXMath types in public struct definitions.

---

### GAP-CIN04 — Singleton Pattern Outside EngineContext

**Files**:
- `Cinematic/Sequencer.h` (line 372, `SequencerManager::GetInstance()`)

**Impact**: `SequencerManager` uses the singleton pattern but is not registered in `EngineContext`. Game modules cannot access the sequencer through the service locator. The singleton creates a hidden global dependency that is difficult to test and cannot be mocked.

**Evidence**: `EngineContext.h` has no `GetSequencer()` method. `IEngineContext` interface does not include sequencer access. Game modules would need to call `Spark::Cinematic::SequencerManager::GetInstance()` directly, breaking the module boundary abstraction.

**What is needed**: Register `SequencerManager` in `EngineContext` and expose it through `IEngineContext`. Allow injection for testing.

---

## Moderate Gaps

### GAP-CIN05 — Entity References via Raw uint32_t

**Files**:
- `Cinematic/Sequencer.h` (lines 170, 199 — `uint32_t targetEntityID`)

**Impact**: `EntityTransformTrack` and `EntityPropertyTrack` reference entities via raw `uint32_t` IDs. These IDs are EnTT entity handles that can be recycled. If a referenced entity is destroyed and its ID is reused, the sequencer will animate the wrong entity with no error. There is no validation that the entity still exists when the sequence runs.

**Evidence**: `uint32_t targetEntityID = 0` in both track types. No `World&` reference is held to validate entity existence. No callback or notification when a referenced entity is destroyed.

**What is needed**: Use a stable entity reference (e.g., UUID from the engine's UUID system) or validate entity existence each frame during playback. Log warnings when a referenced entity is missing.

---

### GAP-CIN06 — No Sequence Blending or Transitions

**Files**:
- `Cinematic/Sequencer.h` (lines 319–328 — playback control)

**Impact**: Sequences can be played, paused, and stopped, but there is no facility to blend between two sequences. Transitioning from gameplay camera to a cutscene camera, or crossfading between two cutscenes, requires manual implementation. Abrupt camera cuts feel jarring.

**What is needed**: Add a blend/crossfade API: `PlaySequenceWithBlend(name, blendDuration)` that interpolates between the current camera state and the sequence's first keyframe over the blend duration.

---

### GAP-CIN07 — No Skip/Fast-Forward with Event Guarantee

**Files**:
- `Cinematic/Sequencer.h` (lines 319–323 — `Play()`, `Pause()`, `Stop()`, `SetTime()`)

**Impact**: There is no `Skip()` method. While `SetTime(duration)` can jump to the end, this skips over `EventCue` and `AudioCue` triggers that fall between the current time and the target time. `GetTriggeredCues(prevTime, currentTime)` (lines 226, 246) only fires cues in a time window — a large time jump would correctly fire all skipped events, but `Stop()` does not trigger remaining events.

**What is needed**: Add a `Skip()` method that fires all remaining event cues in order before stopping the sequence. This ensures game state changes triggered by events (door opens, NPC spawns) are not lost when the player skips a cutscene.

---

## Minor Gaps

### GAP-CIN08 — No Audio Integration with AudioEngine

**Files**:
- `Cinematic/Sequencer.h` (lines 83–89, `AudioCue` struct; lines 215–230, `AudioCueTrack`)

**Impact**: `AudioCueTrack` stores `AudioCue` structs with `soundName` and `volume`, but there is no code that actually plays these sounds through the `AudioEngine`. The track identifies *when* to play sounds but does not connect to the audio system. Game code must manually handle `GetTriggeredCues()` and forward to `AudioEngine`.

**What is needed**: Either integrate `AudioEngine` playback directly into `Sequence::Update()`, or provide a default audio callback that is set automatically when the `AudioEngine` is available.

---

### GAP-CIN09 — Documented API Methods Not Present

**Files**:
- `Cinematic/Sequencer.h` (file-level doc comment mentions features not in the API)

**Impact**: The file doc comment mentions capabilities that don't exist in the class API: no `Repeat()` method for looping a section, no `WithInterval()` for timed repetition, no `Blend()` for sequence transitions. While `SetLooping(bool)` exists for full-sequence looping, there is no section repeat or partial loop.

**What is needed**: Either implement the documented features or update the documentation to accurately reflect the current API.

---
