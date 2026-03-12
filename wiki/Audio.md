# Audio

SparkEngine provides a comprehensive audio system built on **XAudio2** (Windows) with **miniaudio** as a cross-platform fallback. It supports 2D and 3D spatial audio with Doppler effects, distance attenuation, and efficient source pooling.

**Source:** `SparkEngine/Source/Audio/AudioEngine.h`

## Initialization

```cpp
AudioEngine audio;
audio.Initialize(32);  // max 32 simultaneous audio sources
```

## Loading Sounds

```cpp
audio.LoadSound("gunshot", "Assets/Audio/gunshot.wav");
audio.LoadSound("music", "Assets/Audio/background.wav");
```

## Playing Sounds

### 2D Audio (UI, music)

```cpp
audio.Play2D("music", 0.8f, true);  // name, volume, loop
```

### 3D Spatial Audio

```cpp
XMFLOAT3 position = {10.0f, 0.0f, 5.0f};
XMFLOAT3 velocity = {0.0f, 0.0f, 0.0f};
audio.Play3D("gunshot", position, velocity, 1.0f, false);
```

## Audio Source Properties

```cpp
struct AudioSource {
    IXAudio2SourceVoice* Voice;
    XMFLOAT3 Position;     // 3D world position
    XMFLOAT3 Velocity;     // For Doppler effects
    float    Volume;       // 0.0 to 1.0+
    float    Pitch;        // 1.0 = normal
    bool     Is3D;         // 3D positioning enabled
    bool     IsLooping;    // Loop continuously
    bool     IsPlaying;    // Currently playing
};
```

## Volume Channels

Three independent volume channels:

| Channel | Description |
|---------|-------------|
| Master | Overall volume multiplier |
| SFX | Sound effects volume |
| Music | Background music volume |

```cpp
audio.SetMasterVolume(1.0f);
audio.SetSFXVolume(0.7f);
audio.SetMusicVolume(0.5f);
```

## 3D Audio Features

- **Distance Attenuation** — Sound volume decreases with distance from the listener
- **Doppler Effect** — Pitch shifts based on relative velocity between source and listener
- **3D Positioning** — Sounds are spatialized relative to the listener position and orientation

### Updating the Listener

```cpp
// Update listener position and orientation each frame
audio.SetListenerPosition(cameraPosition);
audio.SetListenerOrientation(cameraForward, cameraUp);
```

## Audio Mixer

The `AudioMixer` provides mixing buses, reverb zones, and occlusion:

```cpp
AudioMixer mixer;
mixer.Initialize();

// Create mixing buses for independent volume control
mixer.CreateBus("SFX");
mixer.CreateBus("Music");
mixer.CreateBus("Ambient");
mixer.CreateBus("Voice");

// Adjust bus volumes
mixer.SetBusVolume("SFX", 0.8f);
mixer.SetBusMuted("Music", false);
mixer.SetBusSolo("Voice", true);  // Solo: only this bus is audible

// Add a reverb zone (e.g., inside a cave)
ReverbZone cave;
cave.name        = "CaveReverb";
cave.position    = {50.0f, 0.0f, 30.0f};
cave.innerRadius = 10.0f;
cave.outerRadius = 25.0f;
mixer.AddReverbZone(cave, ReverbPreset::Cave);

// Audio occlusion (walls blocking sound)
mixer.SetOcclusionEnabled(true);
float occlusion = mixer.CalculateOcclusion(listenerPos, sourcePos);

// Save and restore mixer snapshots (e.g., for menu vs. gameplay)
mixer.SaveSnapshot("gameplay");
mixer.RestoreSnapshot("gameplay");
```

## Music Manager

The `MusicManager` handles background music, playlists, crossfading, and dynamic music:

```cpp
auto& music = MusicManager::GetInstance();
music.Initialize();

// Play a single track with crossfade
music.Play("Assets/Audio/Music/exploration.wav");
music.CrossfadeTo("Assets/Audio/Music/combat.wav", 2.0f);  // 2-second crossfade

// Playlists
music.RegisterPlaylist("exploration", {"track1.wav", "track2.wav", "track3.wav"});
music.PlayPlaylist("exploration");
music.SetPlaylistMode(PlaylistMode::Shuffle);
music.NextTrack();

// Dynamic music — automatically transitions based on gameplay intensity
music.SetDynamicMusicState(CombatIntensity::Combat);
music.SetCombatIntensity(CombatIntensity::BossFight);
```

## Procedural Sound Effects

Generate common FPS sounds programmatically with `SoundEffectFactory`:

```cpp
// Generate procedural sounds (useful for prototyping)
auto gunshot   = SoundEffectFactory::CreateGunshot();
auto explosion = SoundEffectFactory::CreateExplosion();
auto footstep  = SoundEffectFactory::CreateFootstep();
auto reload    = SoundEffectFactory::CreateReload();
auto pickup    = SoundEffectFactory::CreatePickup();
auto beep      = SoundEffectFactory::CreateBeep(440.0f, 0.5f);  // frequency, duration
```

## Object Pooling

The audio engine uses an object pool for efficient source management. Sources are allocated from the pool when `Play2D`/`Play3D` is called and returned when playback completes.

## ECS Integration

Use `AudioSourceComponent` on entities:

```cpp
auto& src = world.AddComponent<AudioSourceComponent>(entity);
src.soundFile    = "gunshot";
src.volume       = 1.0f;
src.pitch        = 1.0f;
src.loop         = false;
src.is3D         = true;
src.minDistance   = 1.0f;
src.maxDistance   = 50.0f;
```

The `AudioUpdateSystem` automatically updates 3D source positions from [entity](Entity-Component-System) transforms each frame.

## Console Commands

```
audio_info           # Show audio system status
audio_master <vol>   # Set master volume (0.0-1.0)
audio_sfx <vol>      # Set SFX volume
audio_music <vol>    # Set music volume
audio_play <name>    # Play a sound
audio_stop_all       # Stop all sounds
audio_list           # List loaded sounds
audio_sources        # Show active audio sources
```

## Platform Support

| Platform | Backend | Status |
|----------|---------|--------|
| Windows | XAudio2 | Full support |
| Linux | miniaudio | Fallback |
| macOS | miniaudio | Fallback |

---

## See Also

- [Entity Component System](Entity-Component-System) — AudioSourceComponent
- [Asset Pipeline](Asset-Pipeline) — Loading audio assets
- [Cinematic Sequencer](Cinematic-Sequencer) — Audio tracks in cinematics
- [Event System](Event-System) — Triggering audio from game events
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) — Ambient audio for weather and time of day
