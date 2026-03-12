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

## Creating a Cinematic Sequence

```cpp
auto& seqMgr = SequencerManager::GetInstance();

// Create a new sequence
Sequence* intro = seqMgr.CreateSequence("IntroSequence");

// Add a camera track with keyframes
auto* camTrack = intro->AddCameraTrack();
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
auto* npcTrack = intro->AddEntityTransformTrack(npcEntityId);
npcTrack->AddPositionKeyframe(0.0f, {10.0f, 0.0f, 0.0f}, InterpolationMode::Linear);
npcTrack->AddPositionKeyframe(2.5f, {5.0f, 0.0f, 3.0f},  InterpolationMode::Linear);
npcTrack->AddRotationKeyframe(2.5f, {0.0f, 90.0f, 0.0f}, InterpolationMode::Linear);

// Add audio cues
auto* audioTrack = intro->AddAudioCueTrack();
audioTrack->AddCue(AudioCue{0.0f, "cinematic_music", 0.8f, false, {}});
audioTrack->AddCue(AudioCue{2.0f, "footsteps", 1.0f, true, {5.0f, 0.0f, 3.0f}});

// Add subtitles
auto* subTrack = intro->AddSubtitleTrack();
subTrack->AddSubtitle(SubtitleCue{1.0f, 3.5f, "Welcome to SparkCity.", "Narrator", UIColor::White()});
subTrack->AddSubtitle(SubtitleCue{4.0f, 6.0f, "Your mission begins now.", "Commander", UIColor::Yellow()});

// Add event triggers
auto* eventTrack = intro->AddEventTrack();
eventTrack->AddCue(EventCue{5.0f, "enable_player_control", ""});
eventTrack->AddCue(EventCue{5.0f, "start_music", "combat_theme"});

// Add a screen fade
auto* fadeTrack = intro->AddFadeTrack();
fadeTrack->AddKeyframe(FadeKeyframe{0.0f, 1.0f, UIColor::Black(), InterpolationMode::Linear});   // fade from black
fadeTrack->AddKeyframe(FadeKeyframe{1.0f, 0.0f, UIColor::Black(), InterpolationMode::Linear});   // fully visible
fadeTrack->AddKeyframe(FadeKeyframe{5.5f, 0.0f, UIColor::Black(), InterpolationMode::Linear});   // still visible
fadeTrack->AddKeyframe(FadeKeyframe{6.0f, 1.0f, UIColor::Black(), InterpolationMode::Linear});   // fade to black

// Handle sequence events
intro->SetEventCallback([](const std::string& eventName, const std::string& params) {
    if (eventName == "enable_player_control") {
        playerController.SetEnabled(true);
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
CameraState cam    = seq->GetCurrentCameraState();
std::string subtitle = seq->GetCurrentSubtitle();
float fadeOpacity  = seq->GetCurrentFade();

// Check if any cutscene is blocking gameplay
if (seqMgr.IsAnyCutscenePlaying()) {
    DisablePlayerInput();
}

// Update each frame
seqMgr.Update(deltaTime);
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
