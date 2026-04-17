# SparkConsole

SparkConsole is a standalone debug console application that communicates with the SparkEngine runtime via named pipes, providing real-time engine inspection with 200+ commands.

**Source:** `SparkConsole/src/`

---

## Architecture

```
┌──────────────────────────────┐     Named Pipes     ┌──────────────────────────────┐
│        SparkEngine           │ ◄──────────────────► │        SparkConsole          │
│                              │                      │                              │
│  ┌────────────────────────┐  │   Engine Output      │  ┌────────────────────────┐  │
│  │ SimpleConsole           │  │ ──────────────────►  │  │ ConsoleApp              │  │
│  │ (thread-safe logger)   │  │                      │  │ (main loop + display)  │  │
│  └────────────────────────┘  │   User Commands      │  ├────────────────────────┤  │
│                              │ ◄──────────────────── │  │ CommandParser           │  │
│  ┌────────────────────────┐  │                      │  │ (tokenize + parse)     │  │
│  │ Engine Subsystems       │  │                      │  ├────────────────────────┤  │
│  │ (Graphics, Physics,    │  │                      │  │ CommandRegistry         │  │
│  │  Audio, Input, etc.)   │  │                      │  │ (register + dispatch)  │  │
│  └────────────────────────┘  │                      │  └────────────────────────┘  │
└──────────────────────────────┘                      └──────────────────────────────┘
```

### Source Files

| File | Responsibility |
|------|---------------|
| `ConsoleApp.h` | Main application class -- run loop, I/O threads, display, tab completion, aliases |
| `CommandParser.h` | Static utility -- tokenizes command lines respecting quoted strings |
| `CommandRegistry.h` | Command storage -- registers handlers, dispatches commands, provides help |

---

## Overview

SparkConsole runs as a separate process alongside SparkEngine. When SparkEngine starts, it automatically launches SparkConsole and establishes a named pipe connection for bidirectional communication.

### Communication Flow

1. **Engine Output**: SparkEngine writes log messages, diagnostic data, and command results to the output pipe
2. **User Commands**: SparkConsole reads user input, parses it, and either handles it locally or forwards it to the engine via the input pipe
3. **Background Thread**: A dedicated `m_engineInputThread` reads engine output asynchronously, protected by `m_outputMutex`

---

## Running SparkConsole

### Automatic Launch

SparkConsole opens automatically when SparkEngine starts. No manual configuration is required.

### Standalone Mode

```batch
cd build\bin
SparkConsole.exe
```

In standalone mode, SparkConsole provides local diagnostics and built-in commands but cannot control the engine (engine-forwarded commands will report "not connected").

---

## Internal Implementation

### ConsoleApp Class

```cpp
class ConsoleApp {
public:
    ConsoleApp();
    ~ConsoleApp();
    void Run();    // Main execution loop

private:
    // Thread functions
    void ReadEngineInput();     // Background thread: reads engine pipe
    void ReadUserInput();       // Main thread: reads keyboard input

    // Display
    void PrintLog(const std::wstring& msg);
    void PrintEngineLog(const std::wstring& msg);
    void PrintResult(const std::string& result);
    void SetConsoleColor(WORD color);    // Windows
    void SetConsoleColor(int color);     // Linux

    // Command handling
    void ExecuteCommand(const std::string& cmdLine);
    void RegisterDefaultCommands();
    bool ShouldForwardToEngine(const std::string& command);

    // History management
    void AddToHistory(const std::string& cmd);
    std::string GetPreviousCommand();    // Up arrow
    std::string GetNextCommand();        // Down arrow

    // Tab completion
    std::vector<std::string> GetCompletions(const std::string& prefix);
    void HandleTabCompletion(std::string& input);

    // Alias system
    std::string ResolveAlias(const std::string& input);
};
```

### CommandRegistry

The `CommandRegistry` stores all registered commands and dispatches execution:

```cpp
class CommandRegistry {
public:
    struct CommandInfo {
        std::string name;           // Command name (e.g., "physics_info")
        std::string description;    // Human-readable description
        std::string usage;          // Usage string (e.g., "physics_gravity <x y z>")
        CommandHandler handler;     // std::function<std::string(const CommandArgs&)>
    };

    void RegisterCommand(const std::string& name,
                         const std::string& description,
                         const std::string& usage,
                         CommandHandler handler);

    std::string ExecuteCommand(const std::string& name, const CommandArgs& args);
    bool HasCommand(const std::string& name) const;
    std::vector<CommandInfo> GetAllCommands() const;
    std::string GetCommandHelp(const std::string& name) const;
};
```

### CommandParser

The `CommandParser` tokenizes command lines while respecting quoted strings:

```cpp
class CommandParser {
public:
    // Parse "physics_gravity 0 -9.81 0" into name="physics_gravity", args=["0","-9.81","0"]
    static bool ParseCommandLine(const std::string& commandLine,
                                  std::string& commandName,
                                  CommandArgs& args);

    // Tokenize with quote support: 'echo "hello world"' -> ["echo", "hello world"]
    static std::vector<std::string> Tokenize(const std::string& commandLine);
};
```

---

## Command Categories

### Engine Commands

```
help                  # List all available commands with descriptions
engine_status         # Show engine system initialization status (all subsystems)
status                # Quick engine status summary
diag                  # Run full diagnostics (memory, GPU, CPU, subsystems)
fps                   # Show current framerate and frame time
quit                  # Shut down the engine gracefully
```

### Graphics Commands (see [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md))

```
graphics_info                  # GPU adapter, driver version, feature level
render_path <path>             # Set render pipeline: forward, deferred, forward+, clustered
quality <preset>               # Set quality preset: low, medium, high, ultra
wireframe                      # Toggle wireframe rendering mode
ssao <on|off>                  # Toggle screen-space ambient occlusion
ssr <on|off>                   # Toggle screen-space reflections
bloom <on|off>                 # Toggle bloom post-processing
msaa <1|2|4|8>                 # Set MSAA sample count
vsync <on|off>                 # Toggle vertical sync
resolution <w> <h>             # Set render resolution
fullscreen                     # Toggle fullscreen mode
```

### Physics Commands (see [Physics](../subsystems/Physics.md))

```
physics_info                   # Physics world statistics (body count, constraint count, etc.)
physics_gravity <x y z>        # Set gravity vector (default: 0 -9.81 0)
physics_debug <on|off>         # Toggle collision shape debug visualization
physics_pause                  # Pause physics simulation
physics_step                   # Single-step physics by one fixed timestep
physics_material <name>        # Show properties of a named physics material
```

### Audio Commands (see [Audio](../subsystems/Audio.md))

```
audio_info                     # Audio system status (XAudio2 device, channel count)
audio_master <vol>             # Set master volume (0.0-1.0)
audio_sfx <vol>                # Set SFX channel volume (0.0-1.0)
audio_music <vol>              # Set music channel volume (0.0-1.0)
audio_play <name>              # Play a loaded sound by name
audio_stop_all                 # Stop all currently playing sounds
audio_list                     # List all loaded sound resources
audio_sources                  # Show active audio source positions and states
```

### Input Commands (see [Input System](../subsystems/Input-System.md))

```
input_info                     # Input system status (mouse state, key count, gamepad)
input_sensitivity <val>        # Set mouse sensitivity multiplier (0.1-10.0)
input_deadzone <val>           # Set mouse dead zone threshold (0.0-10.0)
input_invert_y                 # Toggle Y-axis inversion for mouse look
input_bindings                 # List all current key bindings
input_log <on|off>             # Toggle input event logging to console
input_stats                    # Show input statistics (press counts, mouse distance)
```

### Scene Commands (see [Scene Management](../subsystems/Scene-Management.md))

```
scene_info                     # Current scene info (node count, lights, cameras)
scene_list                     # List all scene nodes with hierarchy
scene_load <path>              # Load a scene from file
scene_save <path>              # Save current scene to file
scene_clear                    # Clear all scene nodes
```

### Profiling Commands

```
profile_start                  # Start profiling session (begins capturing data)
profile_stop                   # Stop profiling session
profile_report                 # Print profiling report (function timings, call counts)
profile_gpu                    # GPU timing data per pass (geometry, lighting, post-process)
memory_info                    # Memory usage report (system RAM, VRAM, allocations)
frame_time                     # Frame time breakdown (CPU, GPU, present, idle)
```

### Networking Commands (see [Networking](../subsystems/Networking.md))

```
net_info                       # Network status (connected, protocol, session ID)
net_stats                      # Ping, jitter, packet loss, bandwidth
net_connect <host> <port>      # Connect to a game server
net_disconnect                 # Disconnect from current server
```

### Scripting Commands (see [Scripting with AngelScript](../subsystems/Scripting-with-AngelScript.md))

```
script_reload                  # Hot-reload all AngelScript files
script_run <file>              # Execute a script file immediately
script_list                    # List all loaded script modules
```

### Time and Weather Commands

```
time_set <hour>                # Set time of day (0-24, decimal hours)
time_speed <mult>              # Set time progression speed (1.0 = real-time)
weather_set <type>             # Set weather type: clear, rain, snow, fog, storm
weather_intensity <val>        # Set weather intensity (0.0-1.0)
```

---

## Command Routing

When a command is entered, SparkConsole determines whether to handle it locally or forward it to the engine:

```
User Input
    │
    ▼
CommandParser::ParseCommandLine()
    │
    ├─► Local command? (help, clear, alias, history)
    │       │
    │       ▼
    │   CommandRegistry::ExecuteCommand()
    │       │
    │       ▼
    │   Display result locally
    │
    └─► Engine command? (physics_*, graphics_*, audio_*, etc.)
            │
            ▼
        Forward via named pipe to SparkEngine
            │
            ▼
        Engine processes command
            │
            ▼
        Result returned via output pipe
            │
            ▼
        Display result in console
```

### ShouldForwardToEngine()

Commands are forwarded to the engine when they are not registered in the local `CommandRegistry`. Built-in local commands include: `help`, `clear`, `history`, `alias`, `exit`.

---

## Tab Completion

SparkConsole supports tab completion for command names:

1. Press **Tab** to cycle through matching completions
2. The completion list is populated from all registered commands (both local and engine-side)
3. Pressing Tab multiple times cycles through all matches
4. The completion state resets when the input changes

```cpp
// Internal state
std::vector<std::string> m_tabCompletions;
int m_tabIndex = -1;
std::string m_tabPrefix;
```

---

## Command History

- **Up Arrow**: Navigate to previous commands
- **Down Arrow**: Navigate to next commands
- History is stored in `m_commandHistory` (vector of strings)
- Protected by `m_historyMutex` for thread safety
- Maximum history depth is unlimited within a session

---

## Alias System

Define command shortcuts:

```
alias gs "graphics_info"
alias pg "physics_gravity 0 -9.81 0"
alias reset "quality high; vsync on; msaa 4"
```

Aliases are stored in `m_aliases` (`std::unordered_map<std::string, std::string>`) and resolved before command parsing via `ResolveAlias()`.

---

## Extending with Custom Commands

Engine subsystems register their own console commands during initialization. Each subsystem calls `RegisterCommand()` on the engine's console system with:

1. **Command name** -- unique identifier (e.g., `physics_gravity`)
2. **Description** -- human-readable help text
3. **Usage string** -- parameter documentation (e.g., `physics_gravity <x y z>`)
4. **Handler function** -- `std::function<std::string(const CommandArgs&)>` that processes arguments and returns a result string

### Example: Registering a Custom Command

```cpp
// In your subsystem initialization:
console.RegisterCommand(
    "mycommand",
    "Description of what this command does",
    "mycommand <arg1> [arg2]",
    [this](const CommandArgs& args) -> std::string {
        if (args.empty()) return "Error: missing argument";
        // Process command...
        return "Command executed successfully";
    }
);
```

---

## Thread Safety

SparkConsole uses multiple threads for responsive I/O:

| Thread | Purpose | Synchronization |
|--------|---------|----------------|
| Main thread | User input reading, command execution | -- |
| Engine input thread | Reads engine pipe output asynchronously | `m_outputMutex` |
| (both) | Access to command history | `m_historyMutex` |

The `m_running` flag (`std::atomic<bool>`) signals all threads to shut down cleanly.

### Message Buffer

Engine output messages are buffered in `m_messageBuffer` (`std::deque<std::wstring>`) with a maximum size of 1000 entries to prevent unbounded memory growth. Oldest messages are discarded when the limit is reached.

---

## Performance Considerations

- Named pipe communication is low-latency (< 1ms on Windows)
- The engine input thread runs continuously but blocks on pipe reads (no CPU spin)
- Message buffer caps at 1000 entries to bound memory usage
- Tab completion scans all registered commands (typically < 300) -- negligible cost
- Command parsing uses simple tokenization -- no regex or complex grammar

---

## Platform Support

| Platform | Communication | Status |
|----------|--------------|--------|
| Windows | Named Pipes (`HANDLE`) | Fully supported |
| Linux | stdout / stdin (file descriptors) | Basic output only |
| macOS | stdout / stdin | Basic output only |

On non-Windows platforms, `m_consoleOutput` and `m_consoleInput` are integer file descriptors instead of Windows `HANDLE` values. The `SetConsoleColor()` function uses ANSI escape codes instead of `SetConsoleTextAttribute()`.

---

## Error Handling

- **Pipe disconnection**: If the engine process terminates, the engine input thread detects the broken pipe and prints a "connection lost" message. SparkConsole continues in standalone mode.
- **Unknown commands**: Commands not found in the local registry are forwarded to the engine. If the engine also does not recognize them, an "unknown command" error is returned.
- **Malformed arguments**: Each command handler is responsible for validating its arguments and returning descriptive error messages.
- **Thread shutdown**: The destructor joins `m_engineInputThread` after setting `m_running = false`.

---

## Troubleshooting

### "Standalone mode" message

SparkEngine failed to launch or crashed during startup. Check:
- Both executables exist in `build/bin/`
- Run with a debugger attached for error details
- Verify named pipe permissions (run as same user)

### Commands not responding

- Run `engine_status` to check which subsystems initialized successfully
- Verify the engine's main loop is running (CPU usage should be active)
- Check for a deadlock -- if the engine is blocked on a dialog or assertion, pipe communication stalls

### Tab completion not working

- Ensure commands are registered before SparkConsole connects
- Try `help` to verify the command list is populated

### Console colors not displaying (Linux)

- Ensure your terminal supports ANSI escape codes
- Some terminals require `TERM=xterm-256color`

---

## See Also

- [Getting Started](../getting-started/Getting-Started.md) -- Running SparkConsole
- [Troubleshooting](../advanced/Troubleshooting.md) -- Common console issues
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Graphics commands and render paths
- [Physics](../subsystems/Physics.md) -- Physics simulation commands
- [Audio](../subsystems/Audio.md) -- Audio system commands
- [AI and Navigation](../subsystems/AI-and-Navigation.md) -- AI and pathfinding systems
- [Scene Management](../subsystems/Scene-Management.md) -- Scene loading and management
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation.md) -- Weather and terrain systems
- [Input System](../subsystems/Input-System.md) -- Input debugging commands
- [Event System](../subsystems/Event-System.md) -- Event-related commands
