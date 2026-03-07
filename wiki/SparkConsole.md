# SparkConsole

SparkConsole is a standalone debug console application that communicates with the SparkEngine runtime via named pipes, providing real-time engine inspection with 200+ commands.

**Source:** `SparkConsole/src/`

## Overview

SparkConsole runs as a separate process alongside SparkEngine:

```
┌──────────────┐   Named Pipes   ┌──────────────┐
│ SparkEngine  │ ◄─────────────► │ SparkConsole │
│ (game)       │                 │ (debug)      │
└──────────────┘                 └──────────────┘
```

When SparkEngine starts, it automatically launches SparkConsole and establishes a named pipe connection.

## Running SparkConsole

### Automatic

SparkConsole opens automatically when SparkEngine starts.

### Standalone

```batch
cd build\bin
SparkConsole.exe
```

In standalone mode, SparkConsole provides local diagnostics but cannot control the engine.

## Command Categories

### Engine Commands

```
help              # List all available commands
engine_status     # Show engine system initialization status
status            # Quick engine status
diag              # Run full diagnostics
fps               # Show current framerate
quit              # Shut down the engine
```

### Graphics Commands (see [Rendering and Graphics](Rendering-and-Graphics))

```
graphics_info              # GPU and adapter information
render_path <path>         # Set render pipeline (forward/deferred/forward+/clustered)
quality <preset>           # Set quality (low/medium/high/ultra)
wireframe                  # Toggle wireframe rendering
ssao <on|off>              # Toggle SSAO
ssr <on|off>               # Toggle SSR
bloom <on|off>             # Toggle bloom
msaa <1|2|4|8>             # Set MSAA level
vsync <on|off>             # Toggle vertical sync
resolution <w> <h>         # Set resolution
fullscreen                 # Toggle fullscreen
```

### Physics Commands (see [Physics](Physics))

```
physics_info               # Physics world statistics
physics_gravity <x y z>    # Set gravity
physics_debug <on|off>     # Toggle debug draw
physics_pause              # Pause simulation
physics_step               # Single-step physics
physics_material <name>    # Show material properties
```

### Audio Commands (see [Audio](Audio))

```
audio_info                 # Audio system status
audio_master <vol>         # Set master volume (0.0-1.0)
audio_sfx <vol>            # Set SFX volume
audio_music <vol>          # Set music volume
audio_play <name>          # Play a sound
audio_stop_all             # Stop all sounds
audio_list                 # List loaded sounds
audio_sources              # Show active sources
```

### Input Commands

```
input_info                 # Input system status
input_sensitivity <val>    # Set mouse sensitivity
input_deadzone <val>       # Set dead zone
input_invert_y             # Toggle Y inversion
input_bindings             # List key bindings
input_log <on|off>         # Toggle input logging
input_stats                # Input statistics
```

### Scene Commands (see [Scene Management](Scene-Management))

```
scene_info                 # Current scene info
scene_list                 # List all nodes
scene_load <path>          # Load a scene
scene_save <path>          # Save scene
scene_clear                # Clear scene
```

### Profiling Commands

```
profile_start              # Start profiling
profile_stop               # Stop profiling
profile_report             # Print profiling report
profile_gpu                # GPU timing data
memory_info                # Memory usage report
frame_time                 # Frame time breakdown
```

### Networking Commands

```
net_info                   # Network status
net_stats                  # Ping, jitter, packet loss
net_connect <host> <port>  # Connect to server
net_disconnect             # Disconnect
```

### Scripting Commands

```
script_reload              # Hot-reload all scripts
script_run <file>          # Execute a script file
script_list                # List loaded scripts
```

### Time and Weather Commands (see [Terrain and Procedural Generation](Terrain-and-Procedural-Generation))

```
time_set <hour>            # Set time of day
time_speed <mult>          # Set time speed
weather_set <type>         # Set weather
weather_intensity <val>    # Set intensity
```

## Extending with Custom Commands

Engine subsystems register their own console commands during initialization. The console system supports:
- Command name and description
- Argument parsing
- Tab completion
- Command history

## Platform Support

SparkConsole currently requires Windows (named pipe communication). On Linux, console output goes to `stdout`.

## [Troubleshooting](Troubleshooting)

### "Standalone mode" message

SparkEngine failed to launch or crashed during startup. Check:
- Both executables exist in `build/bin/`
- Run with a debugger attached for error details

### Commands not responding

- Run `engine_status` to check system initialization
- Verify the engine's main loop is running (CPU usage should be active)

---

## See Also

- [Getting Started](Getting-Started) — Running SparkConsole
- [Troubleshooting](Troubleshooting) — Common console issues
- [Rendering and Graphics](Rendering-and-Graphics) — Graphics commands and render paths
- [Physics](Physics) — Physics simulation commands
- [Audio](Audio) — Audio system commands
- [AI and Navigation](AI-and-Navigation) — AI and pathfinding systems
- [Scene Management](Scene-Management) — Scene loading and management
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) — Weather and terrain systems
