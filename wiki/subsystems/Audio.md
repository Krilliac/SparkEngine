# Audio

SparkEngine provides a comprehensive audio system built on **XAudio2** (Windows) with **OpenAL Soft** as a cross-platform fallback for Linux/macOS. It supports 2D and 3D spatial audio with Doppler effects, distance attenuation, efficient source pooling, mix buses, reverb zones, audio occlusion, dynamic music, and procedural sound generation.

**Source Files:**
- `SparkEngine/Source/Audio/AudioEngine.h` -- Core audio engine (XAudio2)
- `SparkEngine/Source/Audio/OpenALAudioEngine.h` -- Cross-platform backend (OpenAL Soft)
- `SparkEngine/Source/Audio/SoundEffect.h` -- WAV loading and procedural sound generation
- `SparkEngine/Source/Audio/AudioMixer.h` -- Mix buses, reverb zones, DSP effects, occlusion
- `SparkEngine/Source/Audio/MusicManager.h` -- Music playback, playlists, dynamic music

## Architecture

```
+------------------------------------------------------------------+
|                        AudioEngine                                |
|  (XAudio2 on Windows, OpenAL Soft on Linux/macOS)                |
|                                                                   |
|  IXAudio2* m_xAudio2           (XAudio2 engine interface)        |
|  IXAudio2MasteringVoice*       (final output stage)              |
|  vector<AudioSource>           (source voice pool)               |
|  unordered_map<SoundEffect>    (loaded sounds by name)           |
|                                                                   |
|  Initialize(maxSources) ──> CreateMasteringVoice                 |
|  LoadSound(name, file)  ──> SoundEffect::LoadFromFile            |
|  PlaySound(name, vol)   ──> GetAvailableSource -> CreateVoice    |
|  PlaySound3D(name, pos) ──> Apply3DAudioToSource                 |
|  Update(dt)             ──> UpdateSources + Update3DAudio        |
+------------------------------------------------------------------+
         |                    |                    |
         v                    v                    v
+----------------+  +------------------+  +------------------+
|  AudioMixer    |  |  MusicManager    |  | SoundEffectFactory|
|                |  |                  |  |                  |
| Mix Buses      |  | Track playback   |  | CreateGunshot()  |
| Reverb Zones   |  | Crossfading      |  | CreateExplosion()|
| DSP Effects    |  | Playlists        |  | CreateFootstep() |
| Occlusion      |  | Dynamic music    |  | CreateBeep(f, d) |
| Snapshots      |  | Combat intensity |  | CreateNoise(d)   |
+----------------+  +------------------+  +------------------+
```

### Signal Flow

```
SoundEffect (WAV data)
       │
       v
AudioSource (IXAudio2SourceVoice)
       │
       ├── Volume: source * bus * master
       ├── 3D: distance attenuation + Doppler + panning
       └── Occlusion: low-pass filter + volume reduction
       │
       v
IXAudio2SubmixVoice (per mix bus: SFX, Music, Voice, Ambient, UI)
       │
       ├── Bus DSP Effects (EQ, Compressor, Delay, Chorus)
       └── Reverb Zone processing
       │
       v
IXAudio2MasteringVoice (final output)
       │
       v
Hardware Audio Output
```

## Initialization

```cpp
AudioEngine audio;
HRESULT hr = audio.Initialize(32);  // max 32 simultaneous audio sources
if (FAILED(hr))
{
    // Handle initialization failure
    Logger::Error("AudioEngine initialization failed: {}", hr);
}
```

The `Initialize` method:
1. Creates the XAudio2 engine via `XAudio2Create()`
2. Creates a mastering voice for final output
3. Allocates the audio source pool (32-64 sources is typical)
4. Initializes 3D audio state (listener position, Doppler scale)

## Loading Sounds

```cpp
audio.LoadSound("gunshot", L"Assets/Audio/gunshot.wav");
audio.LoadSound("music", L"Assets/Audio/background.wav");
audio.LoadSound("footstep", L"Assets/Audio/footstep_concrete.wav");
```

Note: On Windows, `LoadSound` takes a `std::wstring` for the file path. On Linux/macOS with OpenAL, it takes a `std::string`.

### SoundEffect Class

```cpp
class SoundEffect
{
public:
    SoundEffect();
    ~SoundEffect();

    HRESULT LoadFromFile(const std::wstring& filename);
    HRESULT LoadFromMemory(const BYTE* data, DWORD dataSize);
    void Unload();

    const WAVEFORMATEX& GetFormat() const;
    const BYTE* GetData() const;
    DWORD GetDataSize() const;
    bool IsLoaded() const;
    float GetDuration() const;       // seconds
    DWORD GetSampleRate() const;     // Hz
    WORD GetChannels() const;        // 1=mono, 2=stereo
    WORD GetBitsPerSample() const;   // 8, 16, or 24
};
```

Currently supports WAV format audio files. The internal parsing reads RIFF headers, locates `fmt ` and `data` chunks, and stores the raw PCM data in memory.

## Playing Sounds

### 2D Audio (UI, music)

```cpp
AudioSource* src = audio.PlaySound("music", 0.8f, 1.0f, true);
// Parameters: name, volume, pitch, loop
```

### 3D Spatial Audio

```cpp
XMFLOAT3 position = {10.0f, 0.0f, 5.0f};
AudioSource* src = audio.PlaySound3D("gunshot", position, 1.0f, 1.0f, false);
// Parameters: name, position, volume, pitch, loop
```

### Stopping and Pausing

```cpp
audio.StopSound(src);       // Stop a specific source
audio.StopAllSounds();      // Stop all playing sources
audio.PauseAllSounds();     // Pause all (can resume later)
audio.ResumeAllSounds();    // Resume all paused sources
```

## AudioSource Structure

```cpp
struct AudioSource
{
    IXAudio2SourceVoice* Voice; // XAudio2 source voice for playback
    XMFLOAT3 Position;          // 3D world position
    XMFLOAT3 Velocity;          // 3D velocity (for Doppler)
    float Volume;               // Volume level (0.0 to 1.0+)
    float Pitch;                // Pitch multiplier (1.0 = normal)
    bool Is3D;                  // Whether 3D positioning is enabled
    bool IsLooping;             // Loop continuously
    bool IsPlaying;             // Currently active
    SoundEffect* Sound;         // Associated sound effect data
    uint32_t SourceID;          // Unique ID for console tracking
};
```

## Volume Channels

Three independent volume channels with multiplicative stacking:

| Channel | Description | Default |
|---------|-------------|---------|
| Master | Overall volume multiplier applied to all output | 1.0 |
| SFX | Sound effects volume | 1.0 |
| Music | Background music volume | 1.0 |

```cpp
audio.SetMasterVolume(1.0f);
audio.SetSFXVolume(0.7f);
audio.SetMusicVolume(0.5f);

float master = audio.GetMasterVolume();
float sfx    = audio.GetSFXVolume();
float music  = audio.GetMusicVolume();
```

Effective volume for a sound = `sourceVolume * channelVolume * masterVolume`.

## 3D Audio Features

### Distance Attenuation

Sound volume decreases with distance from the listener. The attenuation is controlled by the `distanceScale` parameter.

### Doppler Effect

Pitch shifts based on relative velocity between source and listener. Controlled by `dopplerScale` (default 1.0; set to 0.0 to disable).

### 3D Positioning

Sounds are spatialized relative to the listener position and orientation, producing stereo panning effects.

### Updating the Listener

```cpp
// Must be called every frame for 3D audio to work correctly
audio.SetListenerPosition(cameraPosition);
audio.SetListenerOrientation(cameraForward, cameraUp);
```

### Console 3D Audio Controls

```cpp
audio.Console_SetListenerPosition(x, y, z);
audio.Console_SetListenerOrientation(fwdX, fwdY, fwdZ, upX, upY, upZ);
audio.Console_SetDopplerScale(1.5f);   // 0.0-2.0
audio.Console_SetDistanceScale(2.0f);  // 0.1-10.0
audio.Console_Set3DAudio(true);        // Enable/disable 3D processing
```

## AudioEngine API Reference

### Lifecycle

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `AudioEngine()` | Initialize member defaults |
| Destructor | `~AudioEngine()` | Calls `Shutdown()` |
| `Initialize` | `HRESULT Initialize(size_t maxSources)` | Set up XAudio2, create mastering voice, allocate pool |
| `Update` | `void Update(float deltaTime)` | Update sources, 3D audio, pool management |
| `Shutdown` | `void Shutdown()` | Stop all, release XAudio2 resources |

### Sound Loading

| Method | Signature | Description |
|--------|-----------|-------------|
| `LoadSound` | `HRESULT LoadSound(const std::string& name, const std::wstring& filename)` | Load WAV file |
| `UnloadSound` | `void UnloadSound(const std::string& name)` | Unload and stop instances |
| `GetSound` | `SoundEffect* GetSound(const std::string& name)` | Get loaded sound (nullptr if not found) |

### Playback

| Method | Signature | Description |
|--------|-----------|-------------|
| `PlaySound` | `AudioSource* PlaySound(name, volume, pitch, loop)` | Play 2D sound |
| `PlaySound3D` | `AudioSource* PlaySound3D(name, position, volume, pitch, loop)` | Play 3D sound |
| `StopSound` | `void StopSound(AudioSource* source)` | Stop specific source |
| `StopAllSounds` | `void StopAllSounds()` | Stop all |
| `PauseAllSounds` | `void PauseAllSounds()` | Pause all |
| `ResumeAllSounds` | `void ResumeAllSounds()` | Resume all |

### Volume

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetMasterVolume` | `void SetMasterVolume(float volume)` | Set master (0.0-1.0) |
| `SetSFXVolume` | `void SetSFXVolume(float volume)` | Set SFX channel |
| `SetMusicVolume` | `void SetMusicVolume(float volume)` | Set music channel |
| `GetMasterVolume` | `float GetMasterVolume() const` | Get master |
| `GetSFXVolume` | `float GetSFXVolume() const` | Get SFX |
| `GetMusicVolume` | `float GetMusicVolume() const` | Get music |

### Internal Access

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetActiveSourceCount` | `size_t GetActiveSourceCount() const` | Number of playing sources |
| `GetXAudio2` | `IXAudio2* GetXAudio2() const` | Raw XAudio2 interface |
| `GetMasteringVoice` | `IXAudio2MasteringVoice* GetMasteringVoice() const` | Mastering voice |
| `CreateSubmixVoice` | `IXAudio2SubmixVoice* CreateSubmixVoice(channels, sampleRate)` | Create submix for bus routing |

### AudioMetrics and AudioSettings

```cpp
struct AudioMetrics
{
    size_t activeSources;       // Currently playing
    size_t totalSources;        // Pool capacity
    size_t loadedSounds;        // Loaded sound effects
    float masterVolume;
    float sfxVolume;
    float musicVolume;
    float cpuUsage;             // Audio CPU percentage
    size_t memoryUsage;         // Audio memory (bytes)
    bool is3DEnabled;
    XMFLOAT3 listenerPosition;
    XMFLOAT3 listenerVelocity;
    float dopplerScale;
    float distanceScale;
};

struct AudioSettings
{
    float masterVolume;
    float sfxVolume;
    float musicVolume;
    float dopplerScale;         // 0.0-2.0
    float distanceScale;        // 0.1-10.0
    bool enable3D;
    bool enableReverb;
    bool enableEAX;
    int maxSources;
    XMFLOAT3 listenerPosition;
    XMFLOAT3 listenerVelocity;
    XMFLOAT3 listenerForward;
    XMFLOAT3 listenerUp;
};
```

## Audio Mixer

The `AudioMixer` (in `Spark::Audio` namespace) provides mix buses, reverb zones, DSP effects, audio occlusion, and mixer snapshots.

### Mix Buses

```cpp
AudioMixer mixer;
mixer.Initialize();  // Creates Master, SFX, Music, Voice, Ambient buses

mixer.CreateBus("SFX");
mixer.CreateBus("Music");
mixer.CreateBus("Ambient");
mixer.CreateBus("Voice");
mixer.CreateBus("UI");

mixer.SetBusVolume("SFX", 0.8f);
mixer.SetBusMuted("Music", false);
mixer.SetBusSolo("Voice", true);  // Solo: only this bus is audible

float effectiveVol = mixer.GetEffectiveBusVolume("SFX");  // Cascaded through parents
std::vector<std::string> names = mixer.GetBusNames();
```

### MixBus Structure

```cpp
struct MixBus
{
    std::string name;       // Bus identifier
    float volume = 1.0f;    // Volume multiplier (0.0-1.0)
    bool muted = false;     // Mute all sounds on this bus
    bool solo = false;      // Solo this bus
    std::string parentBus;  // Parent bus name (empty = Master)
};
```

### DSP Effects

Per-bus effect chains for audio processing:

```cpp
enum class DSPEffectType
{
    LowPass,     // Low-pass filter
    HighPass,    // High-pass filter
    BandPass,    // Band-pass filter
    Equalizer,   // Multi-band EQ
    Compressor,  // Dynamic range compressor
    Limiter,     // Hard limiter
    Delay,       // Echo/delay
    Chorus,      // Chorus/flanger
    Distortion   // Distortion/overdrive
};

struct DSPEffect
{
    DSPEffectType type = DSPEffectType::LowPass;
    float param1 = 0.0f;    // Effect-specific parameter 1
    float param2 = 0.0f;    // Effect-specific parameter 2
    float param3 = 0.0f;    // Effect-specific parameter 3
    float wetDry = 1.0f;    // 0.0 = bypass, 1.0 = full effect
    bool enabled = true;
};

// Add effects to a bus
DSPEffect lowPass;
lowPass.type = DSPEffectType::LowPass;
lowPass.param1 = 5000.0f;  // Cutoff frequency in Hz
mixer.AddBusEffect("SFX", lowPass);

mixer.ClearBusEffects("SFX");  // Remove all effects from a bus
```

### Reverb Zones

Spatial volumes that apply reverb to sounds within them:

```cpp
enum class ReverbPreset
{
    None, SmallRoom, MediumRoom, LargeRoom,
    Hall, Cave, Sewer, Outdoor, Forest, Underwater, Custom
};

struct ReverbParameters
{
    ReverbPreset preset = ReverbPreset::None;
    float decayTime = 1.0f;         // Seconds
    float earlyReflections = 0.5f;  // 0.0-1.0
    float lateReverb = 0.5f;        // 0.0-1.0
    float diffusion = 0.7f;         // 0.0-1.0
    float density = 0.8f;           // 0.0-1.0
    float roomSize = 0.5f;          // Virtual room size
    float wetDryMix = 0.3f;         // 0.0=dry, 1.0=fully wet
    float highFreqDamping = 0.5f;   // 0.0-1.0
};

// Create and add a reverb zone
ReverbZone cave;
cave.name = "CaveReverb";
cave.position = {50.0f, 0.0f, 30.0f};
cave.halfExtents = {15.0f, 10.0f, 15.0f};
cave.innerRadius = 10.0f;
cave.outerRadius = 25.0f;
cave.reverb.preset = ReverbPreset::Cave;
cave.reverb.decayTime = 3.0f;
cave.priority = 1;
mixer.AddReverbZone(cave);

// Query reverb at a position
ReverbParameters params = mixer.GetReverbAtPosition(playerPosition);

mixer.RemoveReverbZone("CaveReverb");
```

### Audio Occlusion

Raycasted obstruction between listener and source:

```cpp
mixer.SetOcclusionEnabled(true);

OcclusionResult result = mixer.CalculateOcclusion(listenerPos, sourcePos);
// result.occlusionFactor: 0.0 = unobstructed, 1.0 = fully blocked
// result.lowPassCutoff:   filter frequency in Hz
// result.volumeScale:     volume multiplier from occlusion
// result.wallCount:       number of walls between listener and source
```

Occlusion requires the PhysicsSystem for raycasting. Falls back to unobstructed if physics is unavailable.

### Mixer Snapshots

Save and restore complete mixer state:

```cpp
mixer.SaveSnapshot("gameplay");
mixer.SaveSnapshot("menu");

// Transition to menu audio
mixer.RestoreSnapshot("menu", 1.0f);  // 1-second blend

// Back to gameplay
mixer.RestoreSnapshot("gameplay", 0.5f);
```

## Music Manager

The `MusicManager` (singleton) handles background music, playlists, crossfading, and dynamic music:

```cpp
auto& music = MusicManager::GetInstance();
music.Initialize();
```

### Track Management

```cpp
struct MusicTrack
{
    std::string name;
    std::string filepath;
    float bpm = 120.0f;          // Beats per minute (for sync)
    float loopStartTime = 0.0f;  // Where to loop back to
    float loopEndTime = -1.0f;   // -1 = end of file
    bool loop = true;
    std::string nextTrack;       // Auto-chain to next track
};

MusicTrack track;
track.name = "exploration";
track.filepath = "Assets/Audio/Music/exploration.wav";
track.bpm = 100.0f;
track.loop = true;
music.RegisterTrack(track);
music.UnregisterTrack("exploration");
```

### Playback and Crossfading

```cpp
music.Play("exploration", 1.0f);           // 1-second fade-in
music.CrossfadeTo("combat", 2.0f);        // 2-second crossfade
music.Stop(1.5f);                          // 1.5-second fade-out
music.Pause();
music.Resume();

bool playing = music.IsPlaying();
const std::string& current = music.GetCurrentTrackName();
```

### Playlists

```cpp
enum class PlaylistMode
{
    Sequential, // Play in order
    Shuffle,    // Random order
    Loop,       // Loop entire playlist
    LoopOne     // Loop current track
};

Playlist playlist;
playlist.name = "exploration";
playlist.trackNames = {"track1", "track2", "track3"};
playlist.mode = PlaylistMode::Shuffle;

music.RegisterPlaylist(playlist);
music.PlayPlaylist("exploration");
music.SetPlaylistMode(PlaylistMode::Shuffle);
music.NextTrack();
music.PreviousTrack();
```

### Dynamic Music System

Automatically transitions music based on gameplay intensity:

```cpp
enum class CombatIntensity
{
    Exploration, // Calm ambient music
    LowThreat,  // Tension building
    Combat,      // Full combat music
    BossFight    // Maximum intensity
};

DynamicMusicState state;
state.explorationTrack = "exploration";
state.lowThreatTrack   = "tension";
state.combatTrack      = "combat";
state.bossFightTrack   = "boss_battle";
state.transitionDuration = 2.0f;  // Crossfade time between levels

music.SetDynamicMusicState(state);

// Game events drive intensity changes
music.SetCombatIntensity(CombatIntensity::Combat);
music.SetCombatIntensity(CombatIntensity::BossFight);

CombatIntensity current = music.GetCombatIntensity();
```

## Procedural Sound Effects

Generate common FPS sounds programmatically with `SoundEffectFactory`:

```cpp
auto gunshot   = SoundEffectFactory::CreateGunshot();
auto explosion = SoundEffectFactory::CreateExplosion();
auto footstep  = SoundEffectFactory::CreateFootstep();
auto reload    = SoundEffectFactory::CreateReload();
auto pickup    = SoundEffectFactory::CreatePickup();
auto beep      = SoundEffectFactory::CreateBeep(440.0f, 0.5f);  // frequency, duration
auto sine      = SoundEffectFactory::CreateSine(880.0f, 1.0f);  // pure sine wave
auto noise     = SoundEffectFactory::CreateNoise(0.3f);          // white noise
```

All factory methods return `std::unique_ptr<SoundEffect>` in standard PCM WAV format at 44100 Hz sample rate.

| Factory Method | Parameters | Description |
|----------------|-----------|-------------|
| `CreateBeep` | `freq=440Hz, dur=0.5s` | Square wave beep tone |
| `CreateSine` | `freq=440Hz, dur=1.0s` | Pure sine wave |
| `CreateNoise` | `dur=1.0s` | White noise |
| `CreateGunshot` | none | Procedural gunshot with envelope |
| `CreateExplosion` | none | Low-frequency rumble + high-frequency crack |
| `CreateFootstep` | none | Footstep impact sound |
| `CreateReload` | none | Metallic click reload sound |
| `CreatePickup` | none | Pleasant item collection sound |

## Object Pooling

The audio engine uses an object pool for efficient source management. Sources are pre-allocated during `Initialize()` and reused:

1. When `PlaySound` / `PlaySound3D` is called, `GetAvailableSource()` retrieves an idle source from the pool.
2. When playback completes or `StopSound` is called, `ReturnSource()` marks the source as available.
3. If no sources are available, the engine may stop the oldest playing source to free one up.

Pool size is set by the `maxSources` parameter in `Initialize()`. Typical values: 32 for small games, 64 for open-world.

## OpenAL Backend (Linux/macOS)

On non-Windows platforms, `OpenALAudioEngine` provides the same feature set:

```cpp
// Compiled only when !SPARK_PLATFORM_WINDOWS
Spark::Audio::OpenALAudioEngine audio;
audio.Initialize(32);
audio.LoadSound("gunshot", "Assets/Audio/gunshot.wav");  // std::string, not wstring
auto* src = audio.PlaySound("gunshot", 1.0f, 1.0f, false);
audio.SetListenerPosition({0.0f, 0.0f, 0.0f});
audio.Update(deltaTime);
```

Uses `Spark::Audio::Float3` instead of `XMFLOAT3` for positions/velocities. The interface mirrors the XAudio2 `AudioEngine` so game code can use a platform abstraction layer.

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

The `AudioUpdateSystem` automatically updates 3D source positions from [entity](Entity-Component-System.md) transforms each frame, keeping spatial audio in sync with entity movement.

## Console Commands

```
audio_info           # Show audio system status and metrics
audio_master <vol>   # Set master volume (0.0-1.0)
audio_sfx <vol>      # Set SFX volume
audio_music <vol>    # Set music volume
audio_play <name>    # Play a loaded sound by name
audio_stop_all       # Stop all playing sounds
audio_list           # List all loaded sounds with durations
audio_sources        # Show active audio source count and details
```

### Console Integration API

| Method | Signature | Description |
|--------|-----------|-------------|
| `Console_SetMasterVolume` | `void Console_SetMasterVolume(float volume)` | Set master via console |
| `Console_SetSFXVolume` | `void Console_SetSFXVolume(float volume)` | Set SFX via console |
| `Console_SetMusicVolume` | `void Console_SetMusicVolume(float volume)` | Set music via console |
| `Console_PlayTestSound` | `uint32_t Console_PlayTestSound(name, is3D)` | Play test sound, returns source ID |
| `Console_StopSound` | `void Console_StopSound(uint32_t sourceID)` | Stop by source ID |
| `Console_StopAllSounds` | `void Console_StopAllSounds()` | Stop all |
| `Console_ListSounds` | `std::string Console_ListSounds() const` | List loaded sounds |
| `Console_GetMetrics` | `AudioMetrics Console_GetMetrics() const` | Get audio metrics |
| `Console_GetSettings` | `AudioSettings Console_GetSettings() const` | Get current settings |
| `Console_ApplySettings` | `void Console_ApplySettings(const AudioSettings&)` | Apply settings |
| `Console_ResetToDefaults` | `void Console_ResetToDefaults()` | Reset all to defaults |
| `Console_RefreshAudio` | `void Console_RefreshAudio()` | Force audio refresh |
| `Console_GetSourceInfo` | `std::string Console_GetSourceInfo(uint32_t)` | Get source details |
| `Console_RegisterStateCallback` | `void Console_RegisterStateCallback(function)` | State change notifications |

## Error Handling

| Scenario | Behavior |
|----------|----------|
| `Initialize` fails (no audio device) | Returns `E_FAIL`; all subsequent Play calls return `nullptr` |
| `LoadSound` with invalid file path | Returns `E_FAIL`; sound is not added to the map |
| `LoadSound` with non-WAV file | Returns `E_FAIL`; WAV parsing fails |
| `PlaySound` with unknown name | Returns `nullptr` |
| `PlaySound` with no available sources | May stop oldest source; returns `nullptr` if impossible |
| `StopSound` with `nullptr` | No-op (safe) |
| `SetMasterVolume` with negative value | Clamped to 0.0 |
| Missing OpenAL on Linux | `Initialize` returns `false`; engine runs without audio |

## Performance

- Source pool avoids per-play allocation. Pre-allocate enough sources during `Initialize`.
- WAV data is stored in memory after loading. For large files (music tracks), consider streaming.
- 3D audio calculations (`Apply3DAudioToSource`) run per active 3D source per frame. Keep 3D source count reasonable (under 20 simultaneous).
- Audio occlusion raycasts are expensive. Limit `maxRays` in `OcclusionSettings` (default 4).
- Mixer bus volume cascading is O(depth) per bus, which is negligible in practice.
- `SoundEffectFactory` generates sounds on the calling thread. Generate procedural sounds during loading, not during gameplay.

## Thread Safety

- `AudioEngine` uses `std::mutex` (`m_metricsMutex`) for thread-safe metrics access via `GetMetricsThreadSafe()`.
- `Console_GetMetrics()` is safe to call from any thread.
- All playback methods (`PlaySound`, `StopSound`, etc.) must be called from the **main thread**.
- `OpenALAudioEngine` uses a mutex (`m_mutex`) for source pool access but playback should still be main-thread only.
- `MusicManager` is not thread-safe; call all methods from the main thread.
- `AudioMixer` is not thread-safe; call `Update()` from the main thread.

## Platform Support

| Platform | Backend | 3D Audio | Mixer | Music | Status |
|----------|---------|----------|-------|-------|--------|
| Windows | XAudio2 | Full | Full | Full | Production |
| Linux | OpenAL Soft | Full | Full | Full | Supported |
| macOS | OpenAL Soft | Full | Full | Full | Experimental |

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| No sound output | Initialize not called or failed | Check `Initialize()` return value |
| Silent after scene change | `StopAllSounds` called, new sounds not started | Re-trigger sounds in new scene |
| 3D audio sounds flat | Listener not updated | Call `SetListenerPosition/Orientation` each frame |
| Audio crackling | Too many sources or update not called | Reduce sources; ensure `Update(dt)` every frame |
| Music crossfade glitch | `MusicManager::Update` not called | Call `music.Update(deltaTime)` every frame |
| Reverb not applying | Listener outside reverb zone | Check zone radii and position |
| Occlusion not working | Physics system unavailable | Ensure `ENABLE_PHYSX=ON` and physics initialized |
| OpenAL not found (Linux) | Library not installed | `sudo apt install libopenal-dev` |
| WAV loading fails | File not found or wrong format | Verify path and WAV format (PCM only) |
| Source pool exhausted | Too many simultaneous sounds | Increase `maxSources` or prioritize sounds |

---

## See Also

- [Entity Component System](Entity-Component-System.md) -- AudioSourceComponent
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) -- Loading audio assets
- [Cinematic Sequencer](../gameplay-tools/Cinematic-Sequencer.md) -- Audio tracks in cinematics
- [Event System](Event-System.md) -- Triggering audio from game events
- [Day Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md) -- Ambient audio for weather and time of day
- [Dialogue System](Dialogue-System.md) -- Voice clip playback integration
- [Physics](Physics.md) -- Required for audio occlusion raycasts
