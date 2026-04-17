# Configuration Reference

Complete reference for SparkEngine settings, console commands, and console variables. This page covers everything you can configure without recompiling the engine.

---

## Settings File

SparkEngine loads settings from an INI file at startup.

**Location** (searched in order):
1. `Resources/Config/settings.ini` (relative to executable)
2. `../Resources/Config/settings.ini` (one level up, common in dev builds)
3. Falls back to defaults if not found

**Format:**

```ini
# Comments start with # or ;
[SectionName]
Key = Value
```

Boolean values accept: `true`/`false`, `1`/`0`, `yes`/`no`, `on`/`off`.

### Managing Settings at Runtime

Use the console to read and modify settings without editing the file:

```
settings_list                         # List all settings
settings_sections                     # List all sections
settings_list Graphics                # List settings in a section
settings_search shadow                # Search by pattern
settings_get Graphics WindowWidth     # Get a specific value
settings_set Graphics WindowWidth 1920  # Change a value
settings_save                         # Save to disk
settings_reload                       # Reload from disk
settings_reset                        # Reset everything to defaults
```

---

## Settings by Category

### Graphics

| Key | Default | Description |
|-----|---------|-------------|
| `WindowWidth` | 1280 | Window width in pixels |
| `WindowHeight` | 720 | Window height in pixels |
| `Fullscreen` | false | Fullscreen mode |
| `VSync` | true | Vertical sync |
| `AntiAliasing` | 4 | MSAA sample count (1, 2, 4, 8) |
| `ShadowQuality` | 2 | 0=Off, 1=Low, 2=Medium, 3=High |
| `RenderScale` | 1.0 | Internal resolution scale (0.5–2.0) |
| `HDR` | false | High dynamic range rendering |

### Rendering

| Key | Default | Description |
|-----|---------|-------------|
| `RenderPath` | 1 | 0=Forward, 1=Deferred, 2=Forward+, 3=Clustered |
| `QualityPreset` | 2 | 0=Low, 1=Medium, 2=High, 3=Ultra, 4=Custom |
| `MaxTextureSize` | 2048 | Maximum texture dimension |
| `AnisotropicFiltering` | true | Enable anisotropic filtering |
| `AnisotropyLevel` | 16 | Anisotropy level (1–16) |
| `Shadows` | true | Enable shadow rendering |
| `ShadowMapSize` | 2048 | Shadow map resolution |
| `CascadeCount` | 3 | Cascaded shadow map splits |
| `Bloom` | true | Enable bloom effect |
| `SSAO` | false | Screen-space ambient occlusion |
| `TAA` | false | Temporal anti-aliasing |
| `MotionBlur` | false | Motion blur effect |
| `FrustumCulling` | true | Frustum culling optimization |
| `OcclusionCulling` | false | Hardware occlusion queries |
| `LevelOfDetail` | true | LOD system |
| `MaxDrawCalls` | 1000 | Draw call budget |
| `WireframeMode` | false | Wireframe rendering |
| `DebugMode` | false | Render debug overlays |
| `EnableGPUTiming` | false | GPU timing queries |

### Post-Processing — Bloom

| Key | Default | Description |
|-----|---------|-------------|
| `BloomEnabled` | true | Enable bloom |
| `BloomThreshold` | 1.0 | Brightness threshold |
| `BloomIntensity` | 1.0 | Bloom strength |
| `BloomRadius` | 1.0 | Bloom spread |
| `BloomSoftKnee` | 0.5 | Soft knee falloff |
| `BloomIterations` | 6 | Blur iterations (quality) |

### Post-Processing — Tone Mapping

| Key | Default | Description |
|-----|---------|-------------|
| `ToneMappingOperator` | 4 | 0=None, 1=Reinhard, 2=ReinhardJodie, 3=Uncharted2, 4=ACES, 5=AgX, 6=FilmicALU |
| `Exposure` | 1.0 | Exposure multiplier |
| `Gamma` | 2.2 | Display gamma |
| `WhitePoint` | 11.2 | White point for tonemapping |

### Post-Processing — Color Grading

| Key | Default | Description |
|-----|---------|-------------|
| `ColorGradingEnabled` | false | Enable color grading |
| `Temperature` | 0.0 | Color temperature shift |
| `Tint` | 0.0 | Tint shift |
| `Contrast` | 1.0 | Contrast multiplier |
| `Brightness` | 0.0 | Brightness offset |
| `Saturation` | 1.0 | Saturation multiplier |

### SSAO

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | false | Enable SSAO |
| `Radius` | 0.5 | Sample radius (world units) |
| `Intensity` | 1.0 | Occlusion strength |
| `SampleCount` | 16 | Number of samples |
| `Bias` | 0.025 | Depth bias to prevent self-occlusion |
| `Blur` | true | Blur the AO buffer |

### SSR (Screen-Space Reflections)

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | false | Enable SSR |
| `MaxDistance` | 100.0 | Maximum trace distance |
| `MaxSteps` | 32 | Maximum ray-march steps |
| `Thickness` | 0.5 | Surface thickness |
| `FadeStart` | 80.0 | Fade start distance |
| `FadeEnd` | 100.0 | Fade end distance |

### Volumetric Lighting

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | false | Enable volumetrics |
| `SampleCount` | 32 | Ray-march samples |
| `Scattering` | 0.1 | Scattering coefficient |
| `Extinction` | 0.01 | Extinction coefficient |
| `Anisotropy` | 0.3 | Henyey-Greenstein anisotropy (−1 to 1) |

### TAA (Temporal Anti-Aliasing)

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | true | Enable TAA |
| `Quality` | 2 | 0=Low, 1=Medium, 2=High, 3=Ultra |
| `JitterPattern` | 0 | 0=Halton2,3, 1=BlueNoise, 2=Uniform8x, 3=InterleavedGradient |
| `HistoryBlendFactor` | 0.9 | How much to blend with previous frame |
| `Sharpness` | 0.0 | Post-TAA sharpening |

### Motion Blur

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | false | Enable motion blur |
| `Type` | 2 | 0=CameraOnly, 1=PerObject, 2=Combined |
| `Intensity` | 0.5 | Blur strength |
| `SampleCount` | 8 | Blur samples |
| `MaxBlurRadius` | 32.0 | Maximum blur radius in pixels |

### Dynamic Quality Scaler

| Key | Default | Description |
|-----|---------|-------------|
| `Enabled` | true | Enable dynamic quality scaling |
| `TargetFrameTimeMs` | 16.67 | Target frame time (60 FPS) |
| `MinRenderScale` | 0.5 | Minimum render scale |
| `MaxRenderScale` | 1.0 | Maximum render scale |

### Audio

| Key | Default | Description |
|-----|---------|-------------|
| `MasterVolume` | 1.0 | Master volume (0.0–1.0) |
| `SFXVolume` | 0.8 | Sound effects volume |
| `MusicVolume` | 0.6 | Music volume |
| `VoiceVolume` | 1.0 | Voice/dialogue volume |
| `MuteOnFocusLoss` | true | Mute when window loses focus |

### Audio Extended

| Key | Default | Description |
|-----|---------|-------------|
| `DopplerScale` | 1.0 | Doppler effect strength |
| `DistanceScale` | 1.0 | Distance attenuation scale |
| `Enable3D` | true | 3D spatial audio |
| `EnableReverb` | false | Reverb processing |
| `MaxSources` | 32 | Maximum concurrent audio sources |

### Controls

| Key | Default | Description |
|-----|---------|-------------|
| `MouseSensitivity` | 1.0 | Mouse look sensitivity |
| `InvertMouse` | false | Invert Y axis |
| `MouseDeadZone` | 0.0 | Dead zone threshold |
| `RawMouseInput` | false | Use raw mouse input (bypasses OS acceleration) |
| `MouseAcceleration` | false | Enable mouse acceleration |

### Game

| Key | Default | Description |
|-----|---------|-------------|
| `Difficulty` | "Normal" | Game difficulty level |
| `ShowFPS` | true | Show FPS counter |
| `ShowDebugInfo` | false | Show debug overlay |
| `FieldOfView` | 90.0 | Camera field of view (degrees) |

### Physics

| Key | Default | Description |
|-----|---------|-------------|
| `GravityX` / `GravityY` / `GravityZ` | 0 / −20 / 0 | World gravity vector |
| `FixedTimestep` | 0.016667 | Physics step (60 Hz) |
| `MaxSubSteps` | 4 | Maximum sub-steps per frame |
| `DefaultFriction` | 0.5 | Default body friction |
| `DefaultRestitution` | 0.3 | Default body bounciness |
| `DebugDraw` | false | Physics debug visualization |

### AI

| Key | Default | Description |
|-----|---------|-------------|
| `DetectionRange` | 30.0 | AI detection range (units) |
| `AttackRange` | 15.0 | AI attack range |
| `MoveSpeed` | 5.0 | AI movement speed |
| `TurnSpeed` | 180.0 | AI turn speed (degrees/sec) |
| `Accuracy` | 0.7 | AI aiming accuracy (0.0–1.0) |
| `ReactionTime` | 0.3 | AI reaction delay (seconds) |

### Player

| Key | Default | Description |
|-----|---------|-------------|
| `MaxHealth` | 100.0 | Maximum health |
| `MaxArmor` | 100.0 | Maximum armor |
| `MoveSpeed` | 5.0 | Walk speed |
| `JumpHeight` | 3.0 | Jump height |
| `SprintMultiplier` | 2.0 | Sprint speed multiplier |
| `CrouchMultiplier` | 0.5 | Crouch speed multiplier |

### Camera

| Key | Default | Description |
|-----|---------|-------------|
| `DefaultFov` | 90.0 | Default field of view |
| `ZoomedFov` | 45.0 | ADS / zoomed FOV |
| `NearPlane` | 0.1 | Near clip plane |
| `FarPlane` | 1000.0 | Far clip plane |
| `SmoothMovement` | true | Smooth camera interpolation |

### Network

| Key | Default | Description |
|-----|---------|-------------|
| `ServerPort` | 27015 | Default server port |
| `MaxClients` | 32 | Maximum connected clients |
| `ConnectionTimeout` | 10.0 | Connection timeout (seconds) |
| `ReplicationRate` | 20.0 | Network updates per second |
| `EnableCompression` | false | Compress network packets |
| `EnableEncryption` | false | Encrypt network packets |
| `SimulatedLatencyMs` | 0.0 | Simulated latency for testing |
| `SimulatedPacketLoss` | 0.0 | Simulated packet loss (0.0–1.0) |

### Scripting

| Key | Default | Description |
|-----|---------|-------------|
| `HotReloadEnabled` | true | Auto-reload scripts on file change |
| `HotReloadPollInterval` | 1.0 | File watch interval (seconds) |
| `ExecutionTimeoutMs` | 100.0 | Script execution timeout |
| `MaxCallStackDepth` | 64 | Maximum recursion depth |
| `MaxScriptMemoryMB` | 64 | Script memory limit |

### Animation

| Key | Default | Description |
|-----|---------|-------------|
| `DefaultBlendTime` | 0.2 | Animation blend duration |
| `IKSolverIterations` | 10 | IK solver iterations |
| `MaxActiveMontages` | 4 | Maximum concurrent montages |
| `EnableRootMotion` | true | Enable root motion |
| `CompressionQuality` | 2 | 0=None, 1=Low, 2=Medium, 3=High |

### Editor

| Key | Default | Description |
|-----|---------|-------------|
| `GridSize` | 1.0 | Editor grid spacing |
| `SnapToGrid` | true | Snap objects to grid |
| `ShowGrid` | true | Show grid in viewport |
| `GizmoScale` | 1.0 | Gizmo handle size |
| `AutosaveEnabled` | true | Autosave scenes |
| `AutosaveIntervalSeconds` | 300 | Autosave interval (seconds) |
| `UndoHistorySize` | 100 | Undo stack depth |

### Logging

| Key | Default | Description |
|-----|---------|-------------|
| `GlobalLevel` | "Info" | Log level: Trace, Debug, Info, Warn, Error, Fatal, Off |
| `StackTraceLevel` | "Error" | Include stack traces at this level and above |

Per-category overrides (set to empty string to inherit `GlobalLevel`):

`CoreLevel`, `GraphicsLevel`, `PhysicsLevel`, `AudioLevel`, `AILevel`, `AnimationLevel`, `ECSLevel`, `NetworkLevel`, `InputLevel`, `ScriptingLevel`, `SceneLevel`, `SaveLevel`, `CinematicLevel`, `ProceduralLevel`, `EditorLevel`, `GameLevel`

---

## Console Commands

### Engine Core

| Command | Usage | Description |
|---------|-------|-------------|
| `help` | `help [command]` | List commands or show help for a specific command |
| `clear` | `clear` | Clear the console |
| `version` | `version` | Show engine version |
| `engine_subsystems` | `engine_subsystems` | List all active subsystems and their status |
| `metrics` | `metrics` | Comprehensive system metrics |
| `asset_list` | `asset_list` | List all loaded assets |

### Settings

| Command | Usage | Description |
|---------|-------|-------------|
| `settings_get` | `settings_get <section> <key>` | Read a setting |
| `settings_set` | `settings_set <section> <key> <value>` | Change a setting at runtime |
| `settings_save` | `settings_save` | Write settings to disk |
| `settings_reload` | `settings_reload` | Reload from disk |
| `settings_reset` | `settings_reset` | Reset all to defaults |
| `settings_list` | `settings_list [section]` | List settings |
| `settings_search` | `settings_search <pattern>` | Search settings |

### Rendering

| Command | Usage | Description |
|---------|-------|-------------|
| `render_stats` | `render_stats` | Show draw calls, triangles, batches |
| `render_quality` | `render_quality <low\|medium\|high\|ultra>` | Quality preset |
| `wireframe` | `wireframe <on\|off>` | Toggle wireframe |
| `vsync` | `vsync <on\|off>` | Toggle VSync |
| `shadows` | `shadows <on\|off>` | Toggle shadows |
| `hdr` | `hdr <on\|off>` | Toggle HDR |
| `exposure` | `exposure <value>` | Set exposure |
| `screenshot` | `screenshot` | Capture screenshot |

### Shaders

| Command | Usage | Description |
|---------|-------|-------------|
| `shader_list` | `shader_list` | List loaded shaders |
| `shader_reload` | `shader_reload` | Reload all shaders from disk |
| `shader_debug` | `shader_debug <on\|off>` | Debug shader compilation |

### Textures & Materials

| Command | Usage | Description |
|---------|-------|-------------|
| `tex_list` | `tex_list` | List loaded textures |
| `tex_info` | `tex_info <name>` | Texture details |
| `tex_quality` | `tex_quality <low\|medium\|high\|ultra>` | Texture quality |
| `tex_memory` | `tex_memory <MB>` | Texture memory budget |
| `mat_list` | `mat_list` | List loaded materials |
| `mat_info` | `mat_info <name>` | Material details |

### Physics

| Command | Usage | Description |
|---------|-------|-------------|
| `physics_metrics` | `physics_metrics` | Physics performance stats |
| `physics_list` | `physics_list` | List all physics bodies |
| `physics_body_info` | `physics_body_info <id>` | Body details |
| `physics_gravity` | `physics_gravity <x> <y> <z>` | Set gravity |
| `physics_debug` | `physics_debug <on\|off>` | Toggle debug draw |
| `physics_pause` | `physics_pause` | Pause/resume simulation |
| `physics_timestep` | `physics_timestep <seconds>` | Set fixed timestep |
| `physics_raycast` | `physics_raycast` | Cast a test ray |
| `physics_create` | `physics_create <static\|kinematic\|dynamic>` | Create a body |
| `physics_remove` | `physics_remove <id>` | Remove a body |
| `physics_force` | `physics_force <id> <x> <y> <z>` | Apply force |
| `physics_impulse` | `physics_impulse <id> <x> <y> <z>` | Apply impulse |
| `physics_reset` | `physics_reset` | Reset physics world |

### Audio

| Command | Usage | Description |
|---------|-------|-------------|
| `audio_master_volume` | `audio_master_volume <0.0-1.0>` | Master volume |
| `audio_sfx_volume` | `audio_sfx_volume <0.0-1.0>` | SFX volume |
| `audio_music_volume` | `audio_music_volume <0.0-1.0>` | Music volume |
| `audio_3d` | `audio_3d <on\|off>` | Toggle 3D audio |
| `audio_play_test` | `audio_play_test <name> [3d]` | Play test sound |
| `audio_stop_all` | `audio_stop_all` | Stop all sounds |
| `audio_list` | `audio_list` | List loaded sounds |
| `audio_metrics` | `audio_metrics` | Audio stats |
| `audio_doppler` | `audio_doppler <scale>` | Set Doppler scale |

### Time of Day & Weather

| Command | Usage | Description |
|---------|-------|-------------|
| `time_set` | `time_set <0-24>` | Set time (0=midnight, 12=noon) |
| `time_speed` | `time_speed <multiplier>` | Time progression speed |
| `time_info` | `time_info` | Current time info |
| `weather` | `weather <clear\|rain\|snow\|fog\|storm>` | Set weather |

### Save System

| Command | Usage | Description |
|---------|-------|-------------|
| `save_list` | `save_list` | List save slots |
| `save_info` | `save_info <slot>` | Save slot details |
| `quicksave` | `quicksave` | Quick save |
| `quickload` | `quickload` | Quick load |

### Modules & Hot-Reload

| Command | Usage | Description |
|---------|-------|-------------|
| `module_info` | `module_info` | Show loaded module info |
| `module_reload` | `module_reload [name]` | Hot-reload a module |
| `module.hotreload.enable` | `module.hotreload.enable` | Enable auto hot-reload |
| `module.hotreload.disable` | `module.hotreload.disable` | Disable auto hot-reload |
| `module.hotreload.status` | `module.hotreload.status` | Show hot-reload status |

### Fault Isolation

| Command | Usage | Description |
|---------|-------|-------------|
| `fault.status` | `fault.status` | Show fault status for all subsystems |
| `fault.reset` | `fault.reset <name>` | Re-enable a faulted subsystem |
| `fault.reset_all` | `fault.reset_all` | Re-enable all faulted subsystems |
| `fault.autorecovery` | `fault.autorecovery <name> <on\|off>` | Toggle auto-recovery |

---

## FPS Game Module Commands

These commands are available when the SparkGameFPS module is loaded.

### Gameplay

| Command | Usage | Description |
|---------|-------|-------------|
| `god` | `god` | Toggle god mode |
| `noclip` | `noclip` | Toggle noclip |
| `player_tp` | `player_tp <x> <y> <z>` | Teleport player |
| `spawn` | `spawn <type> <x> <y> <z>` | Spawn an object |
| `game_timescale` | `game_timescale <scale>` | Set time scale |
| `game_stats` | `game_stats` | Performance statistics |
| `hud` | `hud [on\|off]` | Toggle HUD |

### Game Modes

| Command | Usage | Description |
|---------|-------|-------------|
| `gamemode` | `gamemode <mode>` | Switch mode |
| `gamemode_info` | `gamemode_info` | Current mode info |

Available modes: `freeplay`, `deathmatch`, `tdm`, `ctf`, `domination`, `elimination`, `gungame`, `koth`, `survival`

### Inventory & Quests

| Command | Usage | Description |
|---------|-------|-------------|
| `inventory` | `inventory` | Show inventory |
| `give` | `give <id> [count]` | Give item |
| `quest_list` | `quest_list` | Active quests |
| `quest_start` | `quest_start <id>` | Start quest |
| `quest_all` | `quest_all` | List all quests |

### Waves & Combat

| Command | Usage | Description |
|---------|-------|-------------|
| `wave_start` | `wave_start` | Start survival waves |
| `wave_status` | `wave_status` | Current wave info |
| `wave_skip` | `wave_skip [number]` | Skip to wave |
| `wave_difficulty` | `wave_difficulty <1.0-3.0>` | Difficulty scale |

### Progression

| Command | Usage | Description |
|---------|-------|-------------|
| `level` | `level` | Show level and XP |
| `xp` | `xp <amount>` | Award XP |
| `loot_status` | `loot_status` | Loot system info |
| `powerup` | `powerup <type>` | Spawn power-up (speed, damage, shield, ammo, xp, invis) |

### Cinematics & Replay

| Command | Usage | Description |
|---------|-------|-------------|
| `seq_list` | `seq_list` | List sequences |
| `seq_play` | `seq_play <name>` | Play sequence |
| `seq_stop` | `seq_stop [name]` | Stop sequence |
| `replay_start` | `replay_start` | Start recording |
| `replay_stop` | `replay_stop` | Stop recording |
| `replay_save` | `replay_save <path>` | Save replay |
| `replay_load` | `replay_load <path>` | Load replay |
| `replay_play` | `replay_play` | Play replay |
| `replay_speed` | `replay_speed <multiplier>` | Playback speed |

### Networking

| Command | Usage | Description |
|---------|-------|-------------|
| `net_host` | `net_host [port] [max_clients]` | Host a server |
| `net_connect` | `net_connect <address> [port]` | Connect to server |
| `net_disconnect` | `net_disconnect` | Disconnect |
| `net_status` | `net_status` | Connection status |
| `net_stats` | `net_stats` | Network statistics |

---

## Console Variables (CVars)

SparkEngine includes a typed CVar system for runtime-tweakable variables. CVars are separate from settings — they are registered in code and accessed via the CVar registry.

### CVar Flags

| Flag | Meaning |
|------|---------|
| `None` | Normal read/write |
| `ReadOnly` | Cannot be modified at runtime |
| `Cheat` | Only available when cheats are enabled |
| `Save` | Persisted to disk |

### Using CVars in C++

```cpp
#include "Utils/ConsoleVariable.h"

// Register a CVar
static CVar<float> cv_fov("r.fov", 90.0f, CVarFlags::Save, "Field of view");

// Read
float fov = cv_fov.Get();

// Write
cv_fov.Set(110.0f);

// With range constraint
static CVar<float> cv_vol("audio.volume", 1.0f, CVarFlags::Save, "Volume", 0.0f, 1.0f);
```

---

## See Also

- [Getting Started](../getting-started/Getting-Started.md) — Build and run the engine
- [Quick-Start Tutorial](../getting-started/Quick-Start-Tutorial.md) — Your first 10 minutes
- [Performance Tips](Performance-Tips.md) — Optimization guide
- [Troubleshooting](Troubleshooting.md) — Common issues and fixes
- [SparkConsole](../gameplay-tools/SparkConsole.md) — Standalone console application
