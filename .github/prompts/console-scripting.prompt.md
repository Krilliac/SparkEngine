# Console & Scripting

Context: `#prompt:copilot-instructions` for project overview.

## Console System

`Spark::SimpleConsole` (`SparkEngine/Source/Utils/SparkConsole.h`) — Thread-safe singleton, 200+ commands.

### API

```cpp
auto& console = Spark::SimpleConsole::GetInstance();

// Commands
console.RegisterCommand("mycommand",
    [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) return "Usage: mycommand <value>";
        return "Set value to " + args[0];
    }, "Description", "Category");

// Logging (thread-safe): INFO, WARNING, ERROR, CRITICAL, TRACE, DEBUG, SUCCESS
console.Log("Message", "INFO");

// Watch variables (polled every frame)
console.AddWatch("FPS", [&]() { return std::to_string(fps); });

// Aliases and programmatic execution
console.CreateAlias("r", "shader_reload");
console.ExecuteCommand("graphics_vsync 1");
```

### Internal Types

| Type | Purpose |
|------|---------|
| `CommandHandler` | `std::function<std::string(const std::vector<std::string>&)>` |
| `CommandInfo` | `{ handler, description, category }` in `m_commands` map |
| `LogEntry` | `{ message, type, timestamp }` in `m_logHistory` deque |
| `WatchEntry` | `{ name, getter, lastValue, active }` polled each `Update()` |

### Features

Tab completion, command history (Up/Down), alias system, log filtering by severity/text, rate-limited logging, color-coded output (Windows console colors).

### Console Window

Windows: `AllocConsole()`, input in separate thread. Linux: `STDOUT_FILENO`/`STDIN_FILENO`.

### SparkConsole External App

`SparkConsole/` — standalone exe via named pipes (`ConsoleProcessManager`). Fullscreen debugging without overlay.

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

`AngelScriptEngine` (`SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h`) — hot-reload scripting with engine API bindings.

### Script Lifecycle (Unity-style)

```angelscript
class PlayerController {
    void Start()       { /* once on spawn */ }
    void Update(float dt) { /* every frame */ }
    void OnCollision(uint otherId) { /* collision callback */ }
}
```

### Features

Hot-reload (save → detect → recompile → rebind), per-file module isolation, engine API bindings (Transform, Physics, Audio, Input), statically typed.

### Engine API Available in Scripts

| Function | Signature | Purpose |
|----------|-----------|---------|
| `print` | `void print(const string& msg)` | Output to debug console |
| `createEntity` | `uint createEntity(const string& name)` | Create a new ECS entity |
| `getTransform` | `Transform@ getTransform(uint entityId)` | Get entity's Transform component |
| `getKeyDown` | `bool getKeyDown(const string& key)` | Key just pressed this frame |
| `getKey` | `bool getKey(const string& key)` | Key currently held down |

Math types: `XMFLOAT3` (3D vector), `XMMATRIX` (4x4 matrix).

### C++ Integration

```cpp
// Initialize
AngelScriptEngine& engine = *AngelScriptEngine::GetInstance();
engine.Initialize();

// Compile from file
engine.CompileScriptFile("Assets/Scripts/EnemyAI.as");

// Compile from string
engine.CompileScriptFromString(
    "class Test { void Start() { print(\"Hello!\"); } }", "InlineModule");

// Attach to entity and drive lifecycle
engine.AttachScript(entity, "EnemyBehavior", "EnemyAI");
engine.CallStart(entity);
engine.CallUpdate(entity, deltaTime);
engine.CallOnCollision(entity, otherEntity);

// Detach
engine.DetachScript(entity);

// Error handling
if (!engine.CompileScriptFile("Broken.as")) {
    std::string err = engine.GetLastError();
}
```

### Adding Custom Script Bindings

```cpp
// Register a global function
engine->RegisterGlobalFunction("void SpawnParticle(float x, float y, float z)",
    asFUNCTION(ScriptSpawnParticle), asCALL_CDECL);

// Register a value type
engine->RegisterObjectType("Vec3", sizeof(Vec3), asOBJ_VALUE);
engine->RegisterObjectProperty("Vec3", "float x", offsetof(Vec3, x));
```

### ECS Integration

```cpp
auto& script = world.AddComponent<Script>(entity);
script.scriptFile = "EnemyAI";
script.className  = "EnemyBehavior";
script.moduleName = "EnemyAI";
```

### Example Script

```angelscript
// Assets/Scripts/EnemyAI.as
class EnemyBehavior {
    float health = 100.0f;
    float speed = 5.0f;

    void Start() {
        print("Enemy spawned with " + health + " HP");
    }

    void Update(float dt) {
        Transform@ t = getTransform(0);
        if (getKey("W")) {
            // Move forward
        }
        if (getKeyDown("F")) {
            health -= 10.0f;
            print("Hit! Health: " + health);
        }
    }

    void OnCollision(uint otherId) {
        print("Collided with entity " + otherId);
    }
}
```

### Setup & Configuration

1. Ensure `ThirdParty/Scripting/angelscript-mirror/` submodule is cloned
2. Hot-reload: enabled when `ENABLE_HOT_RELOAD=ON` (default) — modifying `.as` files triggers recompile
3. Scripts go in `Assets/Scripts/` with `.as` extension
4. Thread safety: all script calls must happen on the main thread

### Script Console Commands

`script_reload`, `script_debug`, `script_performance`, `mod_load <name>`, `mod_list`
