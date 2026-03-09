# SparkEngine Audio — Gap Analysis

> **Scope**: `SparkEngine/Source/Audio/` (AudioEngine, SoundEffect, MusicManager)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Audio/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Audio subsystem is built on XAudio2 and has a working core: sound loading (WAV via `SoundEffect`), 2D/3D audio playback, source voice pooling, volume controls (master/SFX/music), and a `MusicManager` for background music. The `AudioEngine` has comprehensive console integration. However, it is entirely Windows-only and has significant gaps in 3D audio, effects processing, and cross-platform support.

---

## Critical Gaps

### GAP-A01 — Audio Is Entirely Windows-Only (XAudio2)

**Files**:
- `Audio/AudioEngine.h` (XAudio2 and DirectXMath behind `SPARK_PLATFORM_WINDOWS`)
- `Audio/SoundEffect.h`/`.cpp`
- `Audio/MusicManager.h`/`.cpp`

**Impact**: All audio types (`IXAudio2`, `IXAudio2SourceVoice`, `XAUDIO2_BUFFER`) are Windows-only. Linux and macOS builds have zero audio functionality. The `AudioSource` struct directly contains `IXAudio2SourceVoice*`.

**Evidence**: No OpenAL, SDL_mixer, miniaudio, or any cross-platform audio backend exists. Platform guards produce empty stubs on non-Windows.

**What is needed**: Either abstract the audio backend (similar to the RHI pattern) or use a cross-platform library like miniaudio/OpenAL Soft as the backend on Linux/macOS. At minimum, provide a null audio backend that compiles and runs silently.

---

### GAP-A02 — Only WAV Format Supported

**Files**:
- `Audio/SoundEffect.h`/`.cpp` (5 stub patterns in `.cpp`)

**Impact**: `SoundEffect::Load()` only handles WAV files. OGG Vorbis, MP3, FLAC, and other compressed formats are not supported. Music files in WAV format are extremely large (a 3-minute track = ~30MB uncompressed).

**Evidence**: The `SoundEffect.cpp` has 5 stub patterns, likely in format detection/conversion paths.

**What is needed**: Add OGG Vorbis decoding (via stb_vorbis or libvorbis) for compressed sound effects and music. Consider streaming decode for music tracks to avoid loading entire files into memory.

---

## Major Gaps

### GAP-A03 — No Audio Occlusion or Obstruction

**Files**: `Audio/AudioEngine.h`

**Impact**: 3D audio sources are attenuated by distance but not by geometry. A sound playing behind a thick concrete wall sounds identical to one in the open. For an FPS game, audio occlusion is critical for gameplay (hearing enemies through walls, muffled explosions).

**What is needed**: Implement basic audio occlusion via physics raycasts between listener and source. Apply low-pass filter and volume reduction when occluded. XAudio2's built-in filter DSP can be used for the low-pass.

---

### GAP-A04 — No Reverb or Audio Effects (DSP Chain)

**Files**: `Audio/AudioEngine.h`

**Impact**: No reverb zones, no echo, no environmental audio effects. All sounds play dry regardless of the environment (indoor hallway vs. outdoor field vs. cave).

**What is needed**: Use XAudio2 built-in reverb effect (`XAUDIO2FX_REVERB_PARAMETERS`) on a submix voice. Create reverb zones as trigger volumes that blend reverb parameters based on listener position.

---

### GAP-A05 — No Audio Streaming for Long Tracks

**Files**: `Audio/MusicManager.h`/`.cpp`

**Impact**: Music tracks are loaded entirely into memory via `SoundEffect::Load()`. For a game with multiple music tracks, this can consume hundreds of megabytes of RAM.

**What is needed**: Implement streaming audio playback that decodes and buffers audio in chunks (e.g., 1-second buffers). XAudio2's `OnBufferEnd` callback can trigger the next buffer fill.

---

### GAP-A06 — No Sound Prioritization or Voice Limiting

**Files**: `Audio/AudioEngine.h`

**Impact**: The engine uses a voice pool but has no priority system. When all voices are in use, new sounds may fail to play silently. In an FPS with explosions, gunfire, and ambient sounds, voice management is critical.

**What is needed**: Assign priority levels to sounds. When the pool is exhausted, steal the lowest-priority voice. Track voice count per category and enforce per-category limits (e.g., max 8 gunshot voices, max 4 explosion voices).

---

## Moderate Gaps

### GAP-A07 — No Audio Mixing Bus Architecture

**Files**: `Audio/AudioEngine.h`

**Impact**: Volume is controlled at three levels (master, SFX, music) but there is no submix bus hierarchy. Categories like "dialogue", "ambient", "UI", "weapons", "footsteps" cannot be independently controlled.

**What is needed**: Create XAudio2 submix voices for each audio category. Route source voices to the appropriate submix. Expose per-category volume controls to the console and settings.

---

### GAP-A08 — No Sound Attenuation Curves

**Files**: `Audio/AudioEngine.h`

**Impact**: 3D audio uses default distance attenuation. There is no support for custom rolloff curves (linear, logarithmic, custom) or per-sound min/max distance settings.

**What is needed**: Add `AttenuationSettings` (minDistance, maxDistance, rolloffFactor, curve type) to `AudioSource`. Apply via `X3DAudioCalculate` emitter settings.

---

### GAP-A09 — MusicManager Has Basic Crossfade Only

**Files**: `Audio/MusicManager.h`/`.cpp`

**Impact**: Music transitions are limited to basic crossfade. No support for:
- Stinger playback (short musical accents on events)
- Layered music (adding/removing layers based on intensity)
- Beat-synchronized transitions
- Music playlists with shuffle

**What is needed**: At minimum, add stinger support and layered music (common in FPS for combat intensity).

---

### GAP-A10 — No Doppler Effect Implementation

**Files**: `Audio/AudioEngine.h`

**Impact**: `AudioSource` has a `Velocity` field but it is unclear if Doppler calculation is actually applied. For an FPS with fast-moving projectiles and vehicles, Doppler shifting is expected.

**What is needed**: Ensure `X3DAudioCalculate` is called with `X3DAUDIO_CALCULATE_DOPPLER` flag and that source velocity is correctly updated each frame.

---

## Minor Gaps

### GAP-A11 — No Audio Snapshot / State System

**Impact**: No ability to define audio "states" (e.g., "combat", "stealth", "menu") that adjust volumes, effects, and active sounds as a group.

---

### GAP-A12 — No Audio Asset Hot-Reload

**Impact**: Sound files cannot be reloaded at runtime without restarting the engine. Slow iteration for sound designers.

---

### GAP-A13 — Minimal Logging (5 Calls Total)

**Impact**: As noted in the Logging Gap Analysis, the entire audio subsystem has only 5 log calls. XAudio2 initialization failures, voice creation errors, and buffer underruns are not logged.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-A01 | Critical | Windows-only (XAudio2) | No cross-platform audio |
| GAP-A02 | Critical | WAV only | No compressed audio formats |
| GAP-A03 | Major | No audio occlusion | Sound through walls |
| GAP-A04 | Major | No reverb/effects | No environmental audio |
| GAP-A05 | Major | No streaming playback | High memory for music |
| GAP-A06 | Major | No voice prioritization | Sounds fail silently |
| GAP-A07 | Moderate | No mixing bus architecture | Limited volume control |
| GAP-A08 | Moderate | No attenuation curves | Default distance falloff |
| GAP-A09 | Moderate | Basic music system | No stingers/layers |
| GAP-A10 | Moderate | No Doppler effect | Missing audio realism |
| GAP-A11 | Minor | No audio states | No grouped audio control |
| GAP-A12 | Minor | No hot-reload | Slow iteration |
| GAP-A13 | Minor | Minimal logging | Hard to diagnose issues |

---

## Recommended Priority Order

1. **GAP-A02** — OGG Vorbis support (essential for shipping)
2. **GAP-A05** — Streaming playback (memory reduction)
3. **GAP-A06** — Voice prioritization (gameplay stability)
4. **GAP-A03** — Audio occlusion (FPS gameplay quality)
5. **GAP-A04** — Reverb zones (environmental audio)
6. **GAP-A07** — Mixing bus architecture
7. **GAP-A01** — Cross-platform audio (when Linux/macOS are prioritized)
8. Everything else
