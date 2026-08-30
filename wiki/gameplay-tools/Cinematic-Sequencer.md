# Cinematic Sequencer

SparkEngine includes a timeline-based cinematic sequencer for creating in-game cutscenes and scripted events.

**Source:** `SparkEngine/Source/Engine/Cinematic/Sequencer.h`

The sequencer is compiled as part of the engine; there is no separate CMake toggle.

## Overview

The sequencer provides a non-linear timeline editor for:
- Camera movements and cuts
- Entity animations and transformations
- [Audio](../subsystems/Audio.md) cue playback
- [Event triggers](../subsystems/Event-System.md)
- UI transitions
- Subtitle display
- Screen fades

## Architecture

```
SequencerManager (singleton)
  |
  +-- Sequence "IntroSequence"
  |     +-- CameraPathTrack       (camera keyframes with spline interpolation)
  |     +-- EntityTransformTrack  (position/rotation/scale keyframes)
  |     +-- EntityPropertyTrack   (float property animation, e.g. light intensity)
  |     +-- AudioCueTrack         (timed sound effect triggers)
  |     +-- EventTrack            (script/code callback triggers)
  |     +-- SubtitleTrack         (timed subtitle display)
  |     +-- FadeTrack             (screen fade in/out keyframes)
  |
  +-- Sequence "OutroSequence"
  |     +-- ...
  |
  +-- Update(deltaTime)           (advances all playing sequences)
```

All classes reside in the `Spark::Cinematic` namespace.

## Concepts

### Sequence

A `Sequence` is a named collection of tracks that play together over a defined time range. Each sequence has independent playback controls (play, pause, stop, seek, speed, looping) and fires event callbacks at specific times.

### Tracks

Each track controls a specific aspect of the scene:

| Track Type | Class | Controls |
|-----------|-------|----------|
| Camera | `CameraPathTrack` | Camera position, rotation, FOV, roll over time |
| Transform | `EntityTransformTrack` | Entity position, rotation, scale over time |
| Property | `EntityPropertyTrack` | Arbitrary float property (e.g. `light.intensity`, `material.opacity`) |
| Audio | `AudioCueTrack` | Sound effect and music cue timing |
| Event | `EventTrack` | Custom event triggers at specific times |
| Subtitle | `SubtitleTrack` | Timed subtitle/dialogue text display |
| Fade | `FadeTrack` | Screen fade in/out with color control |

All tracks inherit from `SequencerTrack`:

```cpp
class SequencerTrack
{
public:
    virtual ~SequencerTrack() = default;
    virtual TrackType GetType() const = 0;
    virtual const char* GetName() const = 0;
    virtual float GetDuration() const = 0;

    bool enabled = true;
    std::string name;
};
```

### TrackType Enum

```cpp
enum class TrackType
{
    CameraPath,       // Camera animation track
    EntityProperty,   // Animate a float property on an entity
    EntityTransform,  // Animate entity position/rotation/scale
    AudioCue,         // Trigger audio at specific times
    Event,            // Fire script/code callbacks at specific times
    Subtitle,         // Display subtitle text
    Fade              // Screen fade in/out
};
```

### Keyframes

Keyframes define values at specific points in time. The sequencer interpolates between keyframes using one of four modes:

```cpp
enum class InterpolationMode
{
    Step,         // No interpolation -- snap to value instantly
    Linear,       // Linear interpolation between keyframes
    CubicBezier,  // Cubic bezier curve for eased motion
    CatmullRom    // Catmull-Rom spline for smooth camera paths
};
```

## Keyframe and Cue Data Structures

### CameraKeyframe

```cpp
struct CameraKeyframe
{
    float time;                                            // Time in seconds
    XMFLOAT3 position;                                    // World-space camera position
    XMFLOAT3 lookAt;                                      // World-space look-at target
    float fov;                                             // Field of view in degrees
    float roll;                                            // Roll angle in degrees
    InterpolationMode interpolation = InterpolationMode::CatmullRom;
};
```

### VectorKeyframe

```cpp
struct VectorKeyframe
{
    float time;                                            // Time in seconds
    XMFLOAT3 value;                                       // XYZ vector value
    InterpolationMode interpolation = InterpolationMode::Linear;
};
```

### PropertyKeyframe

```cpp
struct PropertyKeyframe
{
    float time;                                            // Time in seconds
    float value;                                           // Scalar float value
    InterpolationMode interpolation = InterpolationMode::Linear;
};
```

### FadeKeyframe

```cpp
struct FadeKeyframe
{
    float time;                                            // Time in seconds
    float opacity;                                         // 0.0 = transparent, 1.0 = fully opaque
    XMFLOAT3 color{0, 0, 0};                              // Fade color (default: black)
    InterpolationMode interpolation = InterpolationMode::Linear;
};
```

### AudioCue

```cpp
struct AudioCue
{
    float time;                                            // Trigger time in seconds
    std::string soundName;                                 // Asset name of the sound
    float volume = 1.0f;                                   // Playback volume [0, 1]
    bool is3D = false;                                     // Spatialized audio
    XMFLOAT3 position{0, 0, 0};                            // 3D position (if is3D)
};
```

### EventCue

```cpp
struct EventCue
{
    float time;                                            // Trigger time in seconds
    std::string eventName;                                 // Event identifier string
    std::string parameters;                                // JSON or key=value parameters
};
```

### SubtitleCue

```cpp
struct SubtitleCue
{
    float startTime;                                       // Display start time
    float endTime;                                         // Display end time
    std::string text;                                      // Subtitle text
    std::string speaker;                                   // Speaker name (for attribution)
    XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f};              // Text color (RGBA)
};
```

## Track Class Reference

### CameraPathTrack

```cpp
class CameraPathTrack : public SequencerTrack
{
public:
    void AddKeyframe(const CameraKeyframe& kf);
    void RemoveKeyframe(int index);
    CameraKeyframe Evaluate(float time) const;
    const std::vector<CameraKeyframe>& GetKeyframes() const;
};
```

The camera track uses Catmull-Rom spline interpolation by default, producing smooth dolly/crane-style camera paths. For hard cuts, set keyframes to `InterpolationMode::Step`.

### EntityTransformTrack

```cpp
class EntityTransformTrack : public SequencerTrack
{
public:
    uint32_t targetEntityID = 0;

    void AddPositionKeyframe(const VectorKeyframe& kf);
    void AddRotationKeyframe(const VectorKeyframe& kf);
    void AddScaleKeyframe(const VectorKeyframe& kf);

    XMFLOAT3 EvaluatePosition(float time) const;
    XMFLOAT3 EvaluateRotation(float time) const;
    XMFLOAT3 EvaluateScale(float time) const;
};
```

### EntityPropertyTrack

```cpp
class EntityPropertyTrack : public SequencerTrack
{
public:
    uint32_t targetEntityID = 0;
    std::string propertyName;    // E.g., "light.intensity", "material.opacity"

    void AddKeyframe(const PropertyKeyframe& kf);
    float Evaluate(float time) const;
    const std::vector<PropertyKeyframe>& GetKeyframes() const;
};
```

### AudioCueTrack

```cpp
class AudioCueTrack : public SequencerTrack
{
public:
    void AddCue(const AudioCue& cue);
    const std::vector<AudioCue>& GetCues() const;
    std::vector<const AudioCue*> GetTriggeredCues(float prevTime, float currentTime) const;
};
```

The `GetTriggeredCues()` method returns cues whose trigger time falls in the `(prevTime, currentTime]` window, ensuring each cue fires exactly once during playback.

### EventTrack

```cpp
class EventTrack : public SequencerTrack
{
public:
    void AddCue(const EventCue& cue);
    const std::vector<EventCue>& GetCues() const;
    std::vector<const EventCue*> GetTriggeredCues(float prevTime, float currentTime) const;
};
```

### SubtitleTrack

```cpp
class SubtitleTrack : public SequencerTrack
{
public:
    void AddSubtitle(const SubtitleCue& cue);
    const SubtitleCue* GetActiveSubtitle(float time) const;
    const std::vector<SubtitleCue>& GetSubtitles() const;
};
```

### FadeTrack

```cpp
class FadeTrack : public SequencerTrack
{
public:
    void AddKeyframe(const FadeKeyframe& kf);
    FadeKeyframe Evaluate(float time) const;
};
```

## Sequence Class Reference

```cpp
enum class SequencePlayState { Stopped, Playing, Paused };

using EventCallback = std::function<void(const std::string& eventName, const std::string& params)>;

class Sequence
{
public:
    explicit Sequence(const std::string& name);

    // Track management
    CameraPathTrack*      AddCameraTrack(const std::string& trackName);
    EntityTransformTrack* AddEntityTransformTrack(const std::string& trackName, uint32_t entityID);
    EntityPropertyTrack*  AddEntityPropertyTrack(const std::string& trackName, uint32_t entityID,
                                                 const std::string& property);
    AudioCueTrack*        AddAudioCueTrack(const std::string& trackName);
    EventTrack*           AddEventTrack(const std::string& trackName);
    SubtitleTrack*        AddSubtitleTrack(const std::string& trackName);
    FadeTrack*            AddFadeTrack(const std::string& trackName);

    // Playback control
    void Play();
    void Pause();
    void Stop();
    void SetTime(float time);
    void SetPlaybackSpeed(float speed);   // 1.0 = normal, 0.5 = half, 2.0 = double
    void SetLooping(bool loop);

    // Per-frame update
    void Update(float deltaTime);

    // Event callback
    void SetEventCallback(EventCallback callback);

    // State queries
    const std::string&  GetName() const;
    float               GetCurrentTime() const;
    float               GetDuration() const;        // Max duration across all tracks
    SequencePlayState   GetPlayState() const;
    float               GetPlaybackSpeed() const;
    bool                IsLooping() const;

    // Current evaluated state (for rendering)
    const CameraKeyframe* GetCurrentCameraState() const;
    const SubtitleCue*    GetCurrentSubtitle() const;
    FadeKeyframe          GetCurrentFade() const;

    // Track access
    const std::vector<std::unique_ptr<SequencerTrack>>& GetTracks() const;
};
```

## Creating a Cinematic Sequence

```cpp
auto& seqMgr = SequencerManager::GetInstance();

// Create a new sequence
Sequence* intro = seqMgr.CreateSequence("IntroSequence");

// Add a camera track with keyframes
auto* camTrack = intro->AddCameraTrack("MainCamera");
camTrack->AddKeyframe(CameraKeyframe{
    0.0f,                              // time
    {0.0f, 10.0f, -20.0f},            // position
    {0.0f, 0.0f, 0.0f},               // lookAt
    60.0f,                             // FOV
    0.0f,                              // roll
    InterpolationMode::CubicBezier     // smooth interpolation
});
camTrack->AddKeyframe(CameraKeyframe{
    3.0f,                              // time
    {5.0f, 3.0f, -5.0f},              // position
    {0.0f, 1.0f, 0.0f},               // lookAt
    45.0f,                             // FOV (zoom in)
    0.0f,
    InterpolationMode::CubicBezier
});

// Add an entity transform track (move an NPC during the cutscene)
auto* npcTrack = intro->AddEntityTransformTrack("NPC_Walk", npcEntityId);
npcTrack->AddPositionKeyframe(VectorKeyframe{0.0f, {10.0f, 0.0f, 0.0f}, InterpolationMode::Linear});
npcTrack->AddPositionKeyframe(VectorKeyframe{2.5f, {5.0f, 0.0f, 3.0f},  InterpolationMode::Linear});
npcTrack->AddRotationKeyframe(VectorKeyframe{2.5f, {0.0f, 90.0f, 0.0f}, InterpolationMode::Linear});

// Add a property track (fade a light in)
auto* lightTrack = intro->AddEntityPropertyTrack("SpotLight", lightEntityId, "light.intensity");
lightTrack->AddKeyframe(PropertyKeyframe{0.0f, 0.0f, InterpolationMode::Linear});
lightTrack->AddKeyframe(PropertyKeyframe{2.0f, 1.0f, InterpolationMode::CubicBezier});

// Add audio cues
auto* audioTrack = intro->AddAudioCueTrack("Music");
audioTrack->AddCue(AudioCue{0.0f, "cinematic_music", 0.8f, false, {}});
audioTrack->AddCue(AudioCue{2.0f, "footsteps", 1.0f, true, {5.0f, 0.0f, 3.0f}});

// Add subtitles
auto* subTrack = intro->AddSubtitleTrack("Dialogue");
subTrack->AddSubtitle(SubtitleCue{1.0f, 3.5f, "Welcome to SparkCity.", "Narrator",
                                  {1.0f, 1.0f, 1.0f, 1.0f}});
subTrack->AddSubtitle(SubtitleCue{4.0f, 6.0f, "Your mission begins now.", "Commander",
                                  {1.0f, 1.0f, 0.0f, 1.0f}});

// Add event triggers
auto* eventTrack = intro->AddEventTrack("GameEvents");
eventTrack->AddCue(EventCue{5.0f, "enable_player_control", ""});
eventTrack->AddCue(EventCue{5.0f, "start_music", "combat_theme"});

// Add a screen fade
auto* fadeTrack = intro->AddFadeTrack("ScreenFade");
fadeTrack->AddKeyframe(FadeKeyframe{0.0f, 1.0f, {0,0,0}, InterpolationMode::Linear});  // fade from black
fadeTrack->AddKeyframe(FadeKeyframe{1.0f, 0.0f, {0,0,0}, InterpolationMode::Linear});  // fully visible
fadeTrack->AddKeyframe(FadeKeyframe{5.5f, 0.0f, {0,0,0}, InterpolationMode::Linear});  // still visible
fadeTrack->AddKeyframe(FadeKeyframe{6.0f, 1.0f, {0,0,0}, InterpolationMode::Linear});  // fade to black

// Handle sequence events
intro->SetEventCallback([](const std::string& eventName, const std::string& params) {
    if (eventName == "enable_player_control") {
        playerController.SetEnabled(true);
    } else if (eventName == "start_music") {
        audioSystem.PlayMusic(params);
    }
});
```

## Playback Control

```cpp
auto& seqMgr = SequencerManager::GetInstance();

// Play a sequence by name
seqMgr.PlaySequence("IntroSequence");

// Control active sequences
seqMgr.PauseSequence("IntroSequence");
seqMgr.StopSequence("IntroSequence");
seqMgr.StopAll();

// Direct sequence control
Sequence* seq = seqMgr.GetSequence("IntroSequence");
seq->Play();
seq->Pause();
seq->Stop();
seq->SetTime(5.0f);            // Seek to 5 seconds
seq->SetPlaybackSpeed(0.5f);   // Half speed
seq->SetLooping(true);         // Loop the sequence

// Query state
SequencePlayState state = seq->GetPlayState();  // Stopped, Playing, or Paused
float currentTime = seq->GetCurrentTime();
float duration    = seq->GetDuration();
bool looping      = seq->IsLooping();

// Read current cinematic state for rendering
const CameraKeyframe* cam = seq->GetCurrentCameraState();
const SubtitleCue* subtitle = seq->GetCurrentSubtitle();
FadeKeyframe fade = seq->GetCurrentFade();

// Check if any cutscene is blocking gameplay
if (seqMgr.IsAnyCutscenePlaying()) {
    DisablePlayerInput();
}

// Update each frame
seqMgr.Update(deltaTime);
```

## SequencerManager Class Reference

```cpp
class SequencerManager
{
public:
    static SequencerManager& GetInstance();

    Sequence* CreateSequence(const std::string& name);
    Sequence* GetSequence(const std::string& name);
    void      RemoveSequence(const std::string& name);

    bool PlaySequence(const std::string& name);
    void StopSequence(const std::string& name);
    void PauseSequence(const std::string& name);
    void StopAll();

    void Update(float deltaTime);
    bool IsAnyCutscenePlaying() const;
    Sequence* GetActiveSequence();

    // Console integration
    std::string Console_ListSequences() const;
    std::string Console_GetSequenceInfo(const std::string& name) const;
    void Console_PlaySequence(const std::string& name);
    void Console_StopSequence(const std::string& name);
    void Console_SetTime(const std::string& name, float time);
};
```

## Internal Implementation

### Update Loop

During `Sequence::Update(deltaTime)`:

1. Advance `m_currentTime` by `deltaTime * m_playbackSpeed`.
2. For each `CameraPathTrack`, evaluate the camera state at `m_currentTime`.
3. For each `EntityTransformTrack`, evaluate and apply position/rotation/scale.
4. For each `EntityPropertyTrack`, evaluate and apply the float value.
5. For each `AudioCueTrack`, call `GetTriggeredCues(m_previousTime, m_currentTime)` and play triggered sounds.
6. For each `EventTrack`, call `GetTriggeredCues(m_previousTime, m_currentTime)` and dispatch via `m_eventCallback`.
7. For each `SubtitleTrack`, query `GetActiveSubtitle(m_currentTime)`.
8. For each `FadeTrack`, evaluate the current fade state.
9. If `m_currentTime >= GetDuration()`, either loop (reset to 0) or stop.
10. Store `m_previousTime = m_currentTime` for the next frame's cue detection.

### Interpolation

- **Step**: Returns the value of the keyframe at or before the current time.
- **Linear**: Computes `lerp(a, b, t)` where `t` is the normalized time between two keyframes.
- **CubicBezier**: Uses cubic Hermite interpolation with automatically computed tangents.
- **CatmullRom**: Uses the `CatmullRomInterpolate()` helper in `CameraPathTrack`, which requires four control points (P0, P1, P2, P3) to produce a smooth path through P1 and P2.

## Camera Cuts

The sequencer supports multiple camera techniques:

| Technique | Implementation |
|-----------|---------------|
| **Smooth transition** | Multiple keyframes with CatmullRom or CubicBezier interpolation |
| **Hard cut** | Two keyframes at the same time or adjacent times with Step interpolation |
| **Dolly/crane shot** | Closely spaced CatmullRom keyframes along a path |
| **Zoom** | Keyframes with different FOV values |
| **Dutch angle** | Keyframes with non-zero roll values |
| **Follow shot** | Entity transform track + camera track with matching movement |

## Integration

Cinematics can be triggered from:
- Game module code (C++)
- [AngelScript scripts](../subsystems/Scripting-with-AngelScript.md)
- Trigger volumes in [scenes](../subsystems/Scene-Management.md)
- [Event System](../subsystems/Event-System.md) callbacks

## Editor Support

The SparkEditor includes a visual timeline editor for creating and editing cinematic sequences. See [SparkEditor](SparkEditor.md) for details on the Animation Timeline panel.

## Console Commands

| Command | Description |
|---------|-------------|
| `seq_list` | List all registered sequences |
| `seq_info <name>` | Show detailed info about a sequence |
| `seq_play <name>` | Play a sequence by name |
| `seq_stop <name>` | Stop a sequence by name |
| `seq_time <name> <seconds>` | Seek to a specific time |

## Performance Considerations

- Keyframes are stored in sorted vectors; evaluation uses binary search for O(log N) lookup.
- Camera spline interpolation is computed per-frame only for actively playing sequences.
- Audio and event cue detection uses the `(prevTime, currentTime]` window to ensure exactly-once triggering.
- Tracks can be individually disabled (`track.enabled = false`) to skip evaluation.

## Thread Safety

All sequencer operations must be called from the main thread. The `SequencerManager::Update()` method is called during the main game loop, after physics and before rendering. The event callback is dispatched synchronously on the main thread.

## Error Handling

- `CreateSequence()` overwrites existing sequences with the same name silently.
- `PlaySequence()` returns `false` if the sequence name is not found.
- `GetSequence()` returns `nullptr` if the name is not found.
- Seeking past the end of a sequence clamps to the duration.
- Adding keyframes at negative times is undefined behavior.

## Troubleshooting

### Sequence does not play

1. Verify the sequence was created with `CreateSequence()` before calling `PlaySequence()`.
2. Check that `SequencerManager::Update(deltaTime)` is called every frame.
3. Ensure `deltaTime` is positive and non-zero.

### Audio cues fire multiple times

1. Ensure `Update()` is called exactly once per frame.
2. Check that `m_previousTime` is being correctly updated (this is handled automatically).
3. Verify no duplicate cues exist at the same time.

### Camera appears to jump

1. Ensure at least 2 keyframes exist in the `CameraPathTrack`.
2. For CatmullRom interpolation, at least 4 keyframes produce the smoothest results.
3. Check for accidental Step interpolation on keyframes that should be smooth.

### Subtitles not appearing

1. Verify `startTime < endTime` for each subtitle cue.
2. Query `GetCurrentSubtitle()` each frame and pass the text to your UI renderer.
3. Check that the subtitle track is enabled.

---

## See Also

- [Animation](../subsystems/Animation.md) -- Animation tracks in sequences
- [Audio](../subsystems/Audio.md) -- Audio cue playback
- [SparkEditor](SparkEditor.md) -- Timeline editor UI
- [Event System](../subsystems/Event-System.md) -- Event triggers from sequences
- [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md) -- Script-driven cinematics
- [Scene Management](../subsystems/Scene-Management.md) -- Scene-based cinematic triggers
