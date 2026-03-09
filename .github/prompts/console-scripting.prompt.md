# Console & Scripting

Context: `#prompt:copilot-instructions` for project overview.

## Console System

`Spark::SimpleConsole` (`SparkEngine/Source/Utils/SparkConsole.h`) — Thread-safe singleton with 200+ runtime commands.

### API

```cpp
auto& console = Spark::SimpleConsole::GetInstance();
console.Initialize();

// Register a command
console.RegisterCommand(
    "mycommand",
    [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: mycommand <value>";
        return "Set value to " + args[0];
    },
    "Description shown in help",
    "MyCategory"  // Groups command in help output
);

// Logging (thread-safe, mutex-protected)
console.Log("Engine started", "INFO");
console.Log("Shader failed to compile", "ERROR");
console.Log("FPS: 60", "DEBUG");
// Types: INFO, WARNING, ERROR, CRITICAL, TRACE, DEBUG, SUCCESS

// Watch variables (polled every frame)
console.AddWatch("FPS", [&]() { return std::to_string(fps); });

// Aliases
console.CreateAlias("r", "shader_reload");

// Execute command programmatically
console.ExecuteCommand("graphics_vsync 1");
```

### Internal Architecture

| Type | Purpose |
|------|---------|
| `CommandHandler` | `std::function<std::string(const std::vector<std::string>&)>` |
| `CommandInfo` | `{ handler, description, category }` — stored in `m_commands` map |
| `LogEntry` | `{ message, type, timestamp }` — stored in `m_logHistory` deque |
| `WatchEntry` | `{ name, getter, lastValue, active }` — polled each `Update()` |

### Features

- **Tab completion**: Press Tab to cycle through matching commands
- **Command history**: Up/Down arrows to navigate previous commands
- **Alias system**: Shorthand for frequently used commands
- **Log filtering**: Filter by severity type or search text
- **Rate-limited logging**: Prevents log spam from high-frequency events
- **Color-coded output**: Windows console colors per severity level

### Console Window

- Created via `AllocConsole()` on Windows
- Runs in separate thread for input processing
- Platform abstraction: Linux uses `STDOUT_FILENO`/`STDIN_FILENO`

### SparkConsole External App

`SparkConsole/` — Standalone console executable communicating via named pipes through `ConsoleProcessManager`. Useful for fullscreen debugging without overlay.

### Complete Command Reference

| Category | Key Commands |
|----------|-------------|
| **Engine** | `engine_status`, `entity_count`, `component_list`, `memory_info` |
| **Graphics** | `graphics_vsync`, `graphics_wireframe`, `shader_reload`, `render_debug`, `graphics_stats` |
| **Audio** | `audio_master_volume`, `audio_sfx_volume`, `audio_debug`, `sound_play` |
| **Physics** | `physics_debug`, `gravity`, `raycast_test`, `collision_stats` |
| **Player** | `player_speed`, `noclip`, `god_mode`, `teleport`, `camera_mode` |
| **Weapons** | `spawn_projectile`, `weapon_stats`, `pool_stats`, `explosion_test` |
| **Editor** | `editor_theme`, `panel_toggle`, `editor_layout_save`, `asset_refresh` |
| **Assets** | `assets_refresh`, `assets_load`, `assets_memory_usage`, `assets_hot_reload` |
| **Debug** | `assert_mode`, `crash_test`, `profile_start`, `memory_snapshot`, `debug_overlay` |
| **Build** | `build_clean`, `build_rebuild`, `build_config`, `build_status` |
| **Testing** | `test_run_all`, `test_run`, `benchmark_start`, `stress_test` |
| **Console** | `help`, `command_list`, `history`, `alias`, `console_theme` |

---

## AngelScript Scripting

`AngelScriptEngine` (`SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h`) — Hot-reload scripting with full engine API bindings.

### Script Lifecycle (Unity-style)

```angelscript
// Assets/Scripts/PlayerController.as
class PlayerController {
    void Start()       { /* called once on spawn */ }
    void Update(float dt) { /* called every frame */ }
    void OnCollision(Entity other) { /* collision callback */ }
    void OnDestroy()   { /* cleanup */ }
}
```

### Features

- **Hot-reload**: Save script → engine detects change → recompiles → re-binds
- **Per-file isolation**: Each `.as` file compiled as separate module
- **Engine API bindings**: Access Transform, Physics, Audio, Input from scripts
- **Type-safe**: AngelScript is statically typed (similar to C++)

### Adding Script Bindings

```cpp
// Register a new function available in scripts
engine->RegisterGlobalFunction("void SpawnParticle(float x, float y, float z)",
    asFUNCTION(ScriptSpawnParticle), asCALL_CDECL);

// Register a new type
engine->RegisterObjectType("Vec3", sizeof(Vec3), asOBJ_VALUE);
engine->RegisterObjectProperty("Vec3", "float x", offsetof(Vec3, x));
```

### Console Commands

| Command | Description |
|---------|-------------|
| `script_reload` | Hot-reload all scripts |
| `script_debug` | Toggle script debug output |
| `script_performance` | Show per-script timing |
| `mod_load <name>` | Load a mod package |
| `mod_list` | List active mods |
