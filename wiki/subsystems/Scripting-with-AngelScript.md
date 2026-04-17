# Scripting with AngelScript

SparkEngine integrates **AngelScript** as its primary gameplay scripting language, supporting hot-reload, entity binding, [visual scripting](Visual-Scripting.md) (60 node types that compile to AngelScript), and full engine API access. **Lua** is also supported as an alternative scripting engine via `LuaScriptEngine`. A [mod system](Mod-System.md) enables user-created content.

**Source:** `SparkEngine/Source/Engine/Scripting/`

## Overview

AngelScript is a statically-typed scripting language with C/C++-like syntax. Scripts are attached to [ECS](Entity-Component-System.md) entities and driven through lifecycle callbacks, similar to Unity's MonoBehaviour pattern. The scripting subsystem comprises three major components:

| Component | Header | Purpose |
|-----------|--------|---------|
| `AngelScriptEngine` | `AngelScriptEngine.h` | Core VM wrapper, compilation, entity binding, lifecycle dispatch |
| `VisualScriptSystem` | `VisualScriptSystem.h` | Node-graph visual scripting with AngelScript compilation backend |
| `ScriptHotReloadManager` | `ScriptHotReload.h` | File watcher and automatic recompilation on save |

## Architecture

```
+---------------------------+
|   AngelScript VM (asIScriptEngine)   |
+---------------------------+
        ^           ^
        |           |
+-------+------+  +-+-------------------+
| CompileFile  |  | CompileFromString   |
| CompileGraph |  | (Visual Script      |
|   (.as)      |  |  compilation)       |
+--------------+  +---------------------+
        |                   |
        v                   v
+------------------------------------------+
|         Module Registry                  |
|  m_modules: map<string, asIScriptModule> |
+------------------------------------------+
        |
        v
+------------------------------------------+
|    Entity Script Binding                 |
|  m_entityScripts: map<EntityID,          |
|                   ScriptInstance>         |
+------------------------------------------+
        |
        v
+------------------------------------------+
|  Lifecycle Dispatch                      |
|  CallStart / CallUpdate / CallOnCollision|
+------------------------------------------+
```

## Writing a Script

Create an `.as` file in `Assets/Scripts/`:

```cpp
// Assets/Scripts/EnemyAI.as

class EnemyBehavior
{
    float health = 100.0f;
    float moveSpeed = 5.0f;

    void Start()
    {
        print("Enemy spawned!");
    }

    void Update(float dt)
    {
        // Game logic runs every frame
        if (getKeyDown("F")) {
            health -= 10.0f;
            print("Enemy hit! Health: " + health);
        }
    }

    void OnCollision(uint entityId)
    {
        print("Collided with entity: " + entityId);
    }
}
```

### Script Class Rules

- Every script class must be declared at the top level of the `.as` file.
- Method names are case-sensitive and must match the lifecycle signatures exactly.
- Member variables are instance-scoped. They are preserved between `Update()` calls and survive hot-reload when possible.
- Scripts can define additional methods beyond the lifecycle callbacks; they are callable from other scripts or from C++ via the AngelScript context API.

## Lifecycle Callbacks

| Callback | Signature | When Called | Notes |
|----------|-----------|------------|-------|
| `Start` | `void Start()` | Once, when the script is first attached | Initialization logic goes here |
| `Update` | `void Update(float dt)` | Every frame, with delta time in seconds | Main game loop tick |
| `OnCollision` | `void OnCollision(uint entityId)` | When the entity collides with another | Requires a collider component |

### Execution Order

Lifecycle callbacks are dispatched during the **Scripting** phase of the ECS update loop. The overall engine execution order is:

```
Physics -> Animation -> AI -> Scripting -> Audio -> Lifecycle -> Render
```

Within the Scripting phase, `Start()` is called before `Update()` for any newly attached scripts. `OnCollision()` is dispatched after the Physics phase delivers collision events.

## Engine API (Available in Scripts)

The `ScriptAPIRegistry` in `ScriptHotReload.h` documents every function registered with the AngelScript VM. They are organized by category:

### Core Functions

| Signature | Description |
|-----------|-------------|
| `void Log(const string &in)` | Print to console |
| `void LogWarning(const string &in)` | Print warning |
| `void LogError(const string &in)` | Print error |
| `float GetDeltaTime()` | Frame delta time in seconds |
| `float GetGameTime()` | Total elapsed game time |

### Entity / ECS Functions

| Signature | Description |
|-----------|-------------|
| `uint CreateEntity()` | Create a new entity |
| `void DestroyEntity(uint)` | Destroy entity by ID |
| `Vector3 GetPosition(uint)` | Get entity world position |
| `void SetPosition(uint, Vector3)` | Set entity world position |
| `Quaternion GetRotation(uint)` | Get entity rotation |
| `void SetRotation(uint, Quaternion)` | Set entity rotation |
| `float GetHealth(uint)` | Get entity health |
| `void SetHealth(uint, float)` | Set entity health |
| `bool IsAlive(uint)` | Check if entity is alive |

### Physics Functions

| Signature | Description |
|-----------|-------------|
| `bool Raycast(Vector3, Vector3, float, RayHit &out)` | Cast ray and get hit info |
| `void ApplyForce(uint, Vector3)` | Apply force to rigidbody |
| `void ApplyImpulse(uint, Vector3)` | Apply impulse to rigidbody |
| `void SetVelocity(uint, Vector3)` | Set linear velocity |
| `Vector3 GetVelocity(uint)` | Get linear velocity |

### Audio Functions

| Signature | Description |
|-----------|-------------|
| `void PlaySound(const string &in)` | Play a sound effect by name |
| `void PlaySoundAt(const string &in, Vector3)` | Play 3D sound at position |
| `void StopSound(const string &in)` | Stop a playing sound |
| `void SetVolume(float)` | Set master volume [0, 1] |

### Input Functions

| Signature | Description |
|-----------|-------------|
| `bool IsKeyDown(int)` | Check if key is currently held |
| `bool IsKeyPressed(int)` | Check if key was just pressed |
| `Vector2 GetMousePosition()` | Get mouse screen position |
| `Vector2 GetMouseDelta()` | Get mouse movement delta |
| `bool IsMouseButtonDown(int)` | Check mouse button state |
| `float GetGamepadAxis(int, int)` | Get gamepad axis value |

### UI Functions

| Signature | Description |
|-----------|-------------|
| `void DrawText(const string &in, float, float, float)` | Draw text at screen position |
| `void DrawProgressBar(float, float, float, float, float)` | Draw progress bar (x, y, w, h, value) |

### Scene Functions

| Signature | Description |
|-----------|-------------|
| `void LoadScene(const string &in)` | Load scene by name |
| `string GetCurrentScene()` | Get current scene name |

### AI Functions

| Signature | Description |
|-----------|-------------|
| `bool FindPath(Vector3, Vector3, array<Vector3> &out)` | Find NavMesh path between two points |
| `Vector3 GetRandomNavPoint()` | Random walkable NavMesh point |

### Animation Functions

| Signature | Description |
|-----------|-------------|
| `void PlayAnimation(uint, const string &in)` | Play animation clip on entity |
| `void SetAnimationSpeed(uint, float)` | Set animation playback speed |

### Debug Functions

| Signature | Description |
|-----------|-------------|
| `void DebugDrawLine(Vector3, Vector3, Color)` | Draw debug line for one frame |
| `void DebugDrawSphere(Vector3, float, Color)` | Draw debug sphere for one frame |

### Math Types

The following math types are registered as value types in AngelScript:

- `Vector2` -- 2D vector (x, y)
- `Vector3` -- 3D vector (x, y, z)
- `Vector4` -- 4D vector (x, y, z, w)
- `Quaternion` -- Rotation quaternion
- `Color` -- RGBA color
- `RayHit` -- Raycast result (position, normal, distance, entityId)

## Using Scripts from C++

### AngelScriptEngine Class Reference

```cpp
class AngelScriptEngine
{
public:
    // Lifecycle
    bool Initialize();
    void Shutdown();

    // Compilation
    bool CompileScriptFile(const std::string& scriptPath);
    bool CompileScriptFromString(const std::string& script, const std::string& moduleName);

    // Entity binding
    bool AttachScript(EntityID entity, const std::string& className, const std::string& moduleName);
    void DetachScript(EntityID entity);

    // Lifecycle dispatch
    void CallStart(EntityID entity);
    void CallUpdate(EntityID entity, float deltaTime);
    void CallOnCollision(EntityID entity, EntityID other);

    // Error handling
    std::string GetLastError() const;

    // Singleton
    static AngelScriptEngine* GetInstance();
};
```

### Compile and Attach

```cpp
AngelScriptEngine& scriptEngine = AngelScriptEngine::GetInstance();
scriptEngine.Initialize();

// Compile a script file
scriptEngine.CompileScriptFile("Assets/Scripts/EnemyAI.as");

// Attach a script class to an entity
scriptEngine.AttachScript(enemyEntity, "EnemyBehavior", "EnemyAI");

// Call lifecycle methods
scriptEngine.CallStart(enemyEntity);         // Called once
scriptEngine.CallUpdate(enemyEntity, dt);    // Called every frame
```

### Compile from String

```cpp
scriptEngine.CompileScriptFromString(
    "class Test { void Start() { print(\"Hello!\"); } }",
    "InlineModule");
```

### Detach Scripts

```cpp
scriptEngine.DetachScript(enemyEntity);
```

### Internal Implementation: ScriptInstance

Each entity-script binding is tracked by a `ScriptInstance` struct:

```cpp
struct ScriptInstance
{
    asIScriptObject*   object;            // The instantiated script object
    asITypeInfo*       typeInfo;           // Type metadata for the script class
    asIScriptContext*   context;           // Execution context for calling methods
    asIScriptFunction* startMethod;       // Cached pointer to Start()
    asIScriptFunction* updateMethod;      // Cached pointer to Update(float)
    asIScriptFunction* onCollisionMethod; // Cached pointer to OnCollision(EntityID)
    std::string        className;
    std::string        moduleName;
};
```

Method pointers are cached at attach time via `CacheScriptMethods()` to avoid repeated lookups during per-frame dispatch.

## ECS Integration

Use the `Script` component to bind scripts to entities:

```cpp
auto& script = world.AddComponent<Script>(entity);
script.scriptFile = "EnemyAI";
script.className  = "EnemyBehavior";
script.moduleName = "EnemyAI";
```

## Visual Scripting System

The `VisualScriptSystem` (in `VisualScriptSystem.h`) provides a node-graph visual scripting frontend that compiles to AngelScript. This enables designers to author gameplay logic without writing code.

### Pin Types

```cpp
enum class PinType : uint8_t
{
    Execution,  // White wire -- controls which node fires next
    Bool,
    Int,
    Float,
    String,
    Vector2,
    Vector3,
    Vector4,
    Entity,     // Entity ID reference
    Any         // Wildcard -- resolved at connection time
};
```

### Node Categories

| Category | Examples |
|----------|---------|
| `Event` | BeginPlay, Tick, OnCollision |
| `FlowControl` | Branch, ForLoop, Sequence |
| `Math` | Add, Multiply, Clamp, Lerp |
| `Logic` | AND, OR, NOT, Compare |
| `String` | Concat, Format, Length |
| `Variable` | Get/Set local or graph variables |
| `Entity` | GetComponent, SpawnEntity, Destroy |
| `Physics` | AddForce, Raycast, SetVelocity |
| `Input` | IsKeyDown, GetAxis, GetMousePos |
| `Audio` | PlaySound, StopSound, SetVolume |
| `Debug` | Print, DrawLine, Log |
| `Custom` | User-registered nodes |

### Graph Workflow

```
1. Create VisualScriptGraph
2. AddNode() -- places nodes from the NodeLibrary
3. AddLink() -- connect output pins to input pins
4. Validate() -- check for type mismatches and cycles
5. CompileToAngelScript() -- generate .as source code
6. Register with AngelScriptEngine for execution
```

### Graph Variables

Graph-level variables can be declared as public (exposed in the editor Inspector) or private:

```cpp
struct GraphVariable
{
    std::string name;
    PinType type = PinType::Float;
    PinValue value;
    bool isPublic = false;  // Exposed to the editor / inspector
};
```

### Serialization

Graphs serialize to JSON for editor persistence:

```cpp
std::string json = graph->SerializeToJSON();
graph->DeserializeFromJSON(json);
```

### Console Commands

| Command | Description |
|---------|-------------|
| `vs_status` | Show visual script system status |
| `vs_list_graphs` | List all loaded visual script graphs |
| `vs_list_nodes` | List all available node templates |

## Hot Reload

The `ScriptHotReloadManager` watches script directories for file changes and automatically recompiles modified scripts without restarting the engine.

### Configuration

```cpp
ScriptHotReloadManager hotReload;
hotReload.AddWatchDirectory("Assets/Scripts/", true);  // recursive
hotReload.SetWatchExtensions({".as", ".angelscript"});
hotReload.SetDebounceMs(300);  // 300ms debounce to avoid rapid re-triggers

hotReload.SetRecompileCallback([&](const std::string& file) -> RecompileResult {
    RecompileResult result;
    result.success = scriptEngine.CompileScriptFile(file);
    result.filePath = file;
    if (!result.success)
        result.errorMessage = scriptEngine.GetLastError();
    return result;
});

hotReload.SetErrorCallback([](const RecompileResult& err) {
    LOG_ERROR("Script error in {}: {}", err.filePath, err.errorMessage);
});

hotReload.Start();
```

### Per-frame polling

```cpp
// In the main game loop:
int recompiled = hotReload.PollChanges();
if (recompiled > 0)
    LOG_INFO("Hot-reloaded {} script(s)", recompiled);
```

### File Change Types

```cpp
enum class FileChangeType
{
    Modified,
    Created,
    Deleted,
    Renamed
};
```

### RecompileResult

```cpp
struct RecompileResult
{
    bool success = false;
    std::string filePath;
    std::string errorMessage;
    int errorLine = 0;
    float compileTimeMs = 0.0f;
};
```

### State Queries

| Method | Returns |
|--------|---------|
| `IsRunning()` | Whether the watcher is active |
| `GetWatchedFileCount()` | Number of tracked script files |
| `GetRecompileCount()` | Total recompilations since start |
| `GetErrorCount()` | Total compilation errors since start |
| `GetRecentErrors()` | Last 10 `RecompileResult` errors |

## Error Handling

Compilation and runtime errors are captured via the AngelScript message callback and stored for retrieval:

```cpp
if (!scriptEngine.CompileScriptFile("Assets/Scripts/Broken.as")) {
    std::string error = scriptEngine.GetLastError();
    LOG_ERROR("Script error: {}", error);
}
```

Errors include the file path, line number, and column where the error occurred. The `ScriptHotReloadManager` additionally tracks per-file error history with `GetRecentErrors()`.

### Common Error Scenarios

| Error | Cause | Resolution |
|-------|-------|------------|
| `Identifier not found` | Using an unregistered function | Check the API registry or spelling |
| `No matching signatures` | Wrong argument types | Verify parameter types match the API table |
| `Module already exists` | Compiling the same module twice | Use unique module names or detach first |
| `Script class not found` | Typo in `className` passed to `AttachScript` | Ensure class name matches the `.as` file exactly |

## Client/Server Script Context

For multiplayer games, scripts can be separated into client-only, server-only, and shared contexts. This prevents server logic from running on clients and vice versa.

```cpp
// On the server:
scriptEngine.SetScriptContext(AngelScriptEngine::ScriptContext::Server);

// On the client:
scriptEngine.SetScriptContext(AngelScriptEngine::ScriptContext::Client);

// Default (runs everywhere):
scriptEngine.SetScriptContext(AngelScriptEngine::ScriptContext::Shared);
```

When set to `Server`, scripts tagged `[client]` are skipped during execution. When set to `Client`, scripts tagged `[server]` are skipped. This enables clean separation of authoritative server logic (e.g., damage calculation, loot drops) from client-only logic (e.g., UI effects, camera shake).

## Module Isolation

Each script file is compiled into a separate AngelScript module, providing namespace isolation between scripts. The module name is derived from the filename by default, or can be specified explicitly when compiling from a string.

Modules are stored in `m_modules: std::unordered_map<std::string, asIScriptModule*>`. Multiple entities can share the same module (and thus the same compiled bytecode) while maintaining independent script object instances.

## Performance Considerations

- **Method caching**: `Start()`, `Update()`, and `OnCollision()` function pointers are cached at attach time. This avoids the cost of name-based lookup on every frame.
- **Context reuse**: Each `ScriptInstance` holds a dedicated `asIScriptContext`. Contexts are created once and reused across calls.
- **Hot-reload debounce**: The default 300ms debounce prevents rapid recompilation while the user is still typing/saving.
- **Visual script compilation**: Compiling a visual script graph to AngelScript is a one-time operation; the resulting AngelScript module runs at the same speed as hand-written scripts.

## Thread Safety

Script contexts are **not thread-safe**. All script calls must happen on the main game loop thread. This includes:

- `CompileScriptFile()` / `CompileScriptFromString()`
- `AttachScript()` / `DetachScript()`
- `CallStart()` / `CallUpdate()` / `CallOnCollision()`
- `ScriptHotReloadManager::PollChanges()`

The `ScriptHotReloadManager` file scanning runs on the main thread during `PollChanges()`. It does not use background threads.

## Console Commands

| Command | Description |
|---------|-------------|
| `script_reload_all` | Force recompile all watched scripts |
| `script_reload_status` | Show hot-reload watcher status |
| `vs_status` | Visual script system status |
| `vs_list_graphs` | List loaded visual script graphs |
| `vs_list_nodes` | List available node templates |

## Troubleshooting

### Script does not execute

1. Verify the script file compiles without errors (check `GetLastError()`).
2. Ensure `AttachScript()` was called with the correct class name and module name.
3. Confirm `CallStart()` and `CallUpdate()` are being called each frame.
4. Check the ECS `Script` component fields match the compiled module.

### Hot-reload not triggering

1. Confirm `ScriptHotReloadManager::Start()` was called.
2. Verify the watch directory path is correct and the file extension is `.as` or `.angelscript`.
3. Increase the debounce interval if saves happen in rapid succession.
4. Call `PollChanges()` every frame in the main loop.

### Visual script compilation fails

1. Run `graph->Validate()` and inspect the `errors` and `warnings` vectors.
2. Check for disconnected execution flow wires (every execution output must connect to an input).
3. Ensure no cycles exist in the data flow graph.
4. Verify all required input pins have connections or default values.

---

## See Also

- [Entity Component System](Entity-Component-System.md) -- Script component
- [Creating a Game Module](../getting-started/Creating-a-Game-Module.md) -- Module lifecycle
- [Event System](Event-System.md) -- Publishing and subscribing to events from scripts
- [Physics](Physics.md) -- Physics API available in scripts
- [Input System](Input-System.md) -- Input API available in scripts
- [Audio](Audio.md) -- Audio API available in scripts
- [Animation](Animation.md) -- Controlling animations from scripts
- [Scene Management](Scene-Management.md) -- Scene operations from scripts
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Visual script graph editor panel
