# 08 — Audio System

**Location:** `SparkEngine/Source/Audio/`

Built on **XAudio2** (Windows), the audio subsystem provides sound loading/playback, 3D spatial audio, advanced mixing with buses and DSP, music management with crossfading and dynamic intensity, reverb zones, and audio occlusion.

---

## AudioEngine — Main Manager

**File:** `SparkEngine/Source/Audio/AudioEngine.h`

### Lifecycle

```cpp
AudioEngine audio;
if (AudioEngine::IsAudioBackendAvailable()) {  // true on Windows only
    audio.Initialize(64);   // Max 64 simultaneous sources
    audio.Update(deltaTime);
    audio.Shutdown();
}
```

### Sound Loading & Playback

```cpp
// Load
audio.LoadSound("gunshot", L"Data/Sounds/gunshot.wav");
audio.LoadSound("footstep", L"Data/Sounds/footstep.wav");

// 2D playback
AudioSource* src = audio.PlaySound("gunshot", 0.8f, 1.0f, false);

// 3D spatial audio
AudioSource* src3D = audio.PlaySound3D("footstep", {10, 0, 5}, 0.6f, 1.0f, false);

// Control
audio.StopSound(src);
audio.StopAllSounds();
audio.PauseAllSounds();
audio.ResumeAllSounds();

// Cleanup
audio.UnloadSound("gunshot");
```

### Volume Control

```cpp
audio.SetMasterVolume(0.8f);
audio.SetSFXVolume(1.0f);
audio.SetMusicVolume(0.5f);
```

### 3D Audio Features

- Distance attenuation (volume decreases with distance)
- Doppler effects (pitch shifts for moving sources)
- Low-pass filtering (obstructed sounds are muffled)
- Listener position/velocity/orientation for spatial calculations

### Console Integration

```cpp
AudioMetrics metrics = audio.Console_GetMetrics();
// metrics: activeSources, totalSources, loadedSounds, masterVolume,
//          cpuUsage, memoryUsage, is3DEnabled, listenerPosition

audio.Console_SetMasterVolume(0.5f);
audio.Console_SetListenerPosition(x, y, z);
audio.Console_SetDopplerScale(1.5f);
audio.Console_Set3DAudio(true);
audio.Console_PlayTestSound("gunshot", true);  // 3D test
```

---

## SoundEffect — WAV File Wrapper

**File:** `SparkEngine/Source/Audio/SoundEffect.h`

```cpp
SoundEffect sfx;
sfx.LoadFromFile(L"Data/Sounds/explosion.wav");

const WAVEFORMATEX& format = sfx.GetFormat();
float duration = sfx.GetDuration();
DWORD sampleRate = sfx.GetSampleRate();
WORD channels = sfx.GetChannels();
```

### Procedural Sound Generation (SoundEffectFactory)

```cpp
auto beep      = SoundEffectFactory::CreateBeep(440.0f, 0.5f);
auto sine      = SoundEffectFactory::CreateSine(880.0f, 1.0f);
auto noise     = SoundEffectFactory::CreateNoise(0.5f);
auto gunshot   = SoundEffectFactory::CreateGunshot();
auto explosion = SoundEffectFactory::CreateExplosion();
auto footstep  = SoundEffectFactory::CreateFootstep();
auto reload    = SoundEffectFactory::CreateReload();
auto pickup    = SoundEffectFactory::CreatePickup();
```

---

## AudioMixer — Advanced Mixing

**File:** `SparkEngine/Source/Audio/AudioMixer.h`

### Mix Buses

Named volume groups with hierarchy:

```cpp
AudioMixer mixer;
mixer.CreateBus("Master");
mixer.CreateBus("SFX", "Master");
mixer.CreateBus("Music", "Master");
mixer.CreateBus("Voice", "Master");
mixer.CreateBus("Ambient", "SFX");
mixer.CreateBus("Weapons", "SFX");

mixer.SetBusVolume("Weapons", 0.8f);
mixer.SetBusMuted("Music", true);
mixer.SetBusSolo("Voice", true);

float effective = mixer.GetEffectiveBusVolume("Weapons");
// = Master * SFX * Weapons = 1.0 * 1.0 * 0.8 = 0.8
```

### Reverb Zones

```cpp
ReverbZone zone;
zone.name = "Cave";
zone.position = {100, 0, 50};
zone.halfExtents = {20, 10, 20};
zone.innerRadius = 10.0f;
zone.outerRadius = 20.0f;
zone.reverb.preset = ReverbPreset::Cave;
zone.reverb.decayTime = 3.0f;
zone.reverb.wetDryMix = 0.5f;
zone.priority = 5;

mixer.AddReverbZone(zone);

// Query reverb at listener position
ReverbParameters params = mixer.GetReverbAtPosition(listenerPos);
```

Presets: `None`, `SmallRoom`, `MediumRoom`, `LargeRoom`, `Hall`, `Cave`, `Sewer`, `Outdoor`, `Forest`, `Underwater`, `Custom`

### Audio Occlusion

```cpp
mixer.SetOcclusionEnabled(true);

OcclusionResult occlusion = mixer.CalculateOcclusion(listenerPos, sourcePos);
// occlusion.occlusionFactor: 0.0 (clear) to 1.0 (fully blocked)
// occlusion.lowPassCutoff: Hz (lower = more muffled)
// occlusion.volumeScale: 0.0 to 1.0
// occlusion.wallCount: number of walls between listener and source
```

### DSP Effects

```cpp
DSPEffect lowPass;
lowPass.type = DSPEffectType::LowPass;
lowPass.param1 = 5000.0f;  // Cutoff frequency
lowPass.wetDry = 0.8f;

mixer.AddBusEffect("Ambient", lowPass);
mixer.ClearBusEffects("Ambient");
```

Effect types: `LowPass`, `HighPass`, `BandPass`, `Equalizer`, `Compressor`, `Limiter`, `Delay`, `Chorus`, `Distortion`

### Snapshots

```cpp
mixer.SaveSnapshot("gameplay");
mixer.SaveSnapshot("cutscene");

mixer.RestoreSnapshot("cutscene", 1.0f);  // Blend over 1 second
mixer.RestoreSnapshot("gameplay", 0.5f);
```

---

## MusicManager — Music Playback

**File:** `SparkEngine/Source/Audio/MusicManager.h`

### Track & Playlist Management

```cpp
MusicManager music;

// Register tracks
music.RegisterTrack({"battle_theme", "Data/Music/battle.wav", 120.0f, 0.0f, -1.0f, true});
music.RegisterTrack({"explore_theme", "Data/Music/explore.wav", 90.0f, 0.0f, -1.0f, true});

// Create playlists
music.CreatePlaylist("combat", {"battle_theme", "battle_intense"}, PlaylistMode::Loop);
music.CreatePlaylist("ambient", {"explore_theme", "forest_theme"}, PlaylistMode::Shuffle);

// Playback
music.Play("explore_theme", 2.0f);         // 2s fade-in
music.CrossfadeTo("battle_theme", 3.0f);   // 3s crossfade
music.Stop(1.5f);                           // 1.5s fade-out

// Playlist
music.PlayPlaylist("combat");
music.NextTrack();
music.PreviousTrack();
music.SetPlaylistMode(PlaylistMode::Shuffle);
```

### Dynamic Music (Combat Intensity)

```cpp
DynamicMusicState dynamicState;
dynamicState.explorationTrack = "explore_theme";
dynamicState.lowThreatTrack = "tension_theme";
dynamicState.combatTrack = "battle_theme";
dynamicState.bossFightTrack = "boss_theme";
dynamicState.transitionDuration = 2.0f;

music.SetDynamicMusicState(dynamicState);

// Game events drive music changes
music.SetCombatIntensity(CombatIntensity::Combat);       // Crossfade to battle
music.SetCombatIntensity(CombatIntensity::Exploration);  // Crossfade back
music.SetCombatIntensity(CombatIntensity::BossFight);    // Crossfade to boss
```

Intensity levels: `Exploration`, `LowThreat`, `Combat`, `BossFight`

### Music-Specific Reverb

```cpp
MusicReverbZone zone;
zone.name = "CaveMusic";
zone.position = {100, 0, 50};
zone.type = MusicReverbZone::Type::Cave;
zone.decayTime = 2.5f;

music.AddReverbZone(zone);
music.UpdateListenerReverbZone(listenerPosition);
```

---

## Platform Support

| Platform | Backend | Status |
|----------|---------|--------|
| Windows | XAudio2 | Full support |
| Linux | Stub | All APIs are no-ops |
| macOS | Stub | All APIs are no-ops |
| Linux (experimental) | OpenAL | `OpenALAudioEngine.h` — basic stub |

---

## Integration with ECS

- **Component**: `AudioSourceComponent` (sound name, volume, pitch, loop, is3D, handle)
- **System**: `AudioUpdateSystem` syncs 3D source positions from `Transform`
- **Listener**: Updated from camera position/orientation each frame
