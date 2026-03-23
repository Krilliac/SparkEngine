# 10 — Scripting System

**Location:** `SparkEngine/Source/Engine/Scripting/`

Dual scripting engine support: **AngelScript** (primary) and **Lua** (optional). Both support hot-reload and per-entity script attachment.

---

## AngelScript Engine

**File:** `SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h`

### Initialization

```cpp
AngelScriptEngine scriptEngine;
scriptEngine.Initialize();

// Register engine bindings
scriptEngine.RegisterFunction("void print(const string &in)", asFUNCTION(ScriptPrint));
scriptEngine.RegisterFunction("uint createEntity()", asFUNCTION(ScriptCreateEntity));
scriptEngine.RegisterFunction("Vec3 getTransform(uint)", asFUNCTION(ScriptGetTransform));
scriptEngine.RegisterFunction("bool getKeyDown(int)", asFUNCTION(ScriptGetKeyDown));
scriptEngine.RegisterFunction("bool getKey(int)", asFUNCTION(ScriptGetKey));
```

### Entity Script Attachment

```cpp
// Attach script to entity
scriptEngine.AttachScript(entityId, "Data/Scripts/enemy_patrol.as", "EnemyPatrol");

// Per-entity lifecycle callbacks
scriptEngine.CallStart(entityId);                    // Once on attach
scriptEngine.CallUpdate(entityId, deltaTime);        // Each frame
scriptEngine.CallOnCollision(entityId, otherEntity); // On physics contact
```

### Hot-Reload

Scripts are automatically recompiled when source files change:

```cpp
scriptEngine.SetHotReloadEnabled(true);
// File watcher detects changes → recompile → rebind to entities
```

### Example AngelScript

```angelscript
class EnemyPatrol {
    float patrolSpeed = 3.0f;
    int currentWaypoint = 0;

    void Start() {
        print("Patrol started!");
    }

    void Update(float dt) {
        Vec3 pos = getTransform(entityId);
        // Move toward waypoint...
    }

    void OnCollision(uint other) {
        print("Collided with entity: " + other);
    }
}
```

### Client/Server Context Separation

The AngelScript VM supports separate script contexts for client and server code, preventing client scripts from accessing server-only state and vice versa.

---

## Lua Script Engine

**File:** `SparkEngine/Source/Engine/Scripting/LuaScriptEngine.h`

Optional secondary scripting language (requires `SPARK_LUA_AVAILABLE`):

```cpp
LuaScriptEngine lua;
lua.Initialize();
lua.LoadScript("Data/Scripts/gameplay.lua");
lua.CallFunction("OnStart");
lua.CallFunction("OnUpdate", deltaTime);
```

### Sandbox

Lua scripts run in a sandboxed environment:
- No file I/O access
- No OS module access
- No package loading
- Whitelisted standard library functions only

### Callbacks

| Callback | When |
|----------|------|
| `OnStart()` | Script loaded |
| `OnUpdate(dt)` | Each frame |
| `OnDestroy()` | Entity destroyed |
| `OnCollisionEnter(other)` | Physics contact begin |
| `OnCollisionExit(other)` | Physics contact end |

---

## Script Hook Manager

**File:** `SparkEngine/Source/Engine/Scripting/ScriptHookManager.h`

Event dispatcher for script lifecycle events:

```cpp
ScriptHookManager hooks;
hooks.RegisterHook("OnEntitySpawned", [](uint32_t entityId) {
    // Initialize script state for new entity
});
hooks.RegisterHook("OnLevelLoaded", [](const std::string& levelName) {
    // Reset script state
});
hooks.FireHook("OnEntitySpawned", newEntityId);
```

---

## Script Hot-Reload

**File:** `SparkEngine/Source/Engine/Scripting/ScriptHotReload.h`

File watcher for automatic recompilation:

```cpp
ScriptHotReload hotReload;
hotReload.Watch("Data/Scripts/");
hotReload.SetOnReload([&](const std::string& path) {
    scriptEngine.ReloadScript(path);
});
hotReload.Update();  // Check for file changes
```

---

## Integration with ECS

- **Component**: `Script` (scriptPath, className, moduleName)
- **System**: Script system queries entities with `Script` component
- **Lifecycle**: `Start()` called on first frame, `Update(dt)` each frame
- **Entity access**: Scripts can query/modify ECS components via registered bindings
