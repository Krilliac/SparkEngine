# Scripting with AngelScript

SparkEngine integrates **AngelScript** as its gameplay scripting language, supporting hot-reload, entity binding, and full engine API access.

**Source:** `SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h`

## Overview

AngelScript is a statically-typed scripting language with C/C++-like syntax. Scripts are attached to [[Entity Component System|ECS]] entities and driven through lifecycle callbacks, similar to Unity's MonoBehaviour pattern.

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

## Lifecycle Callbacks

| Callback | Signature | When Called |
|----------|-----------|------------|
| `Start` | `void Start()` | Once, when the script is first attached |
| `Update` | `void Update(float dt)` | Every frame, with delta time in seconds |
| `OnCollision` | `void OnCollision(uint entityId)` | When the entity collides with another |

## Engine API (Available in Scripts)

### Output

```cpp
void print(const string& msg)    // Output to debug console
```

### Entity Management

```cpp
uint createEntity(const string& name)    // Create a new entity
Transform@ getTransform(uint entityId)   // Get entity's transform
```

### Input

```cpp
bool getKeyDown(const string& key)   // Key just pressed this frame
bool getKey(const string& key)       // Key currently held
```

### Math Types

- `XMFLOAT3` — 3D vector
- `XMMATRIX` — 4x4 matrix

## Using Scripts from C++

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
scriptEngine.CompileScriptString("InlineModule",
    "class Test { void Start() { print(\"Hello!\"); } }");
```

### Detach Scripts

```cpp
scriptEngine.DetachScript(enemyEntity);
```

## ECS Integration

Use the `Script` component to bind scripts to entities:

```cpp
auto& script = world.AddComponent<Script>(entity);
script.scriptFile = "EnemyAI";
script.className  = "EnemyBehavior";
script.moduleName = "EnemyAI";
```

## Hot Reload

AngelScript supports hot-reloading during development. When enabled (`ENABLE_HOT_RELOAD=ON`), modifying a `.as` file triggers recompilation and re-attachment without restarting the engine.

## Error Handling

Compilation and runtime errors are captured:

```cpp
if (!scriptEngine.CompileScriptFile("Assets/Scripts/Broken.as")) {
    std::string error = scriptEngine.GetLastError();
    LOG_ERROR("Script error: " + error);
}
```

## Module Isolation

Each script file is compiled into a separate AngelScript module, providing namespace isolation between scripts.

## Thread Safety

Script contexts are **not thread-safe**. All script calls must happen on the main game loop thread.

---

## See Also

- [[Entity Component System]] — Script component
- [[Creating a Game Module]] — Module lifecycle
- [[Event System]] — Publishing and subscribing to events from scripts
- [[Physics]] — Physics API available in scripts
- [[Input System]] — Input API available in scripts
- [[Audio]] — Audio API available in scripts
- [[Animation]] — Controlling animations from scripts
- [[Scene Management]] — Scene operations from scripts
