# Visual Scripting

## Status: Active (Reimplemented April 2026)

SparkEngine now includes a **unified visual scripting system** — a single, properly wired implementation that compiles node graphs to AngelScript source code.

### Architecture

| Component | File | Purpose |
|-----------|------|---------|
| `VisualScriptCompiler` | `SparkEngine/Source/Engine/Scripting/VisualScriptCompiler.h` | Node graph → AngelScript compiler |
| `VisualScriptPanel` | `SparkEditor/Source/Panels/VisualScriptPanel.h` | ImGui node graph editor |

### How It Works

1. Non-coders build gameplay logic by connecting nodes in the Visual Script panel
2. Clicking **Compile** runs `VisualScriptCompiler::Compile()` which generates a `.as` file
3. The generated file is written to `Assets/Scripts/Generated/`
4. The existing `ScriptHotReload` system detects the change and recompiles automatically
5. **No new runtime** — visual scripts use the same AngelScript pipeline as hand-written scripts

### Node Types (~60 types across 8 categories)

| Category | Examples |
|----------|---------|
| **Events** | OnStart, OnUpdate, OnTriggerEnter, OnDamaged, OnKeyPress, OnCollision |
| **Flow Control** | Branch (If), ForLoop, Sequence |
| **Actions** | SetPosition, PlaySound, PlayAnimation, SpawnEntity, PrintMessage |
| **Math** | Add, Subtract, Multiply, Divide, Lerp, Clamp, Random |
| **Logic** | AND, OR, NOT, Equal, Greater, Less |
| **Getters** | GetKeyDown, GetDeltaTime, GetSelf |
| **Constants** | Float, Int, Bool, String, Vector3 |
| **Variables** | GetVariable, SetVariable |

### Editor UI

- **Left sidebar**: Node palette organized by category
- **Center**: Canvas with pan (middle-mouse), zoom (scroll), node dragging, bezier connections
- **Right sidebar**: Variable definitions + selected node properties
- **Bottom bar**: Compile button, error display

### Example

A visual script with:
- `OnStart` → `PrintMessage("Hello World")`
- `OnUpdate` → `GetKeyDown("Space")` → `Branch` → `ApplyForce(0, 10, 0)`

Generates:
```angelscript
class MyScript
{
    void Start()
    {
        print("Hello World");
    }
    void Update(float dt)
    {
        if (getKeyDown("Space"))
        {
            // ApplyForce...
        }
    }
}
```

### History

The **original** visual scripting system (~7,300 lines) was removed in March 2026 as dead code — two parallel implementations that were never wired in. The current system is a clean reimplementation following the `ShaderGraphCompiler` pattern (node graph → code generation) with a single unified engine + editor implementation.

## See Also

- [Event Response System](Event-Response-System) -- Data-driven "When/If/Then" rules (simpler alternative for common gameplay)
- [Scripting with AngelScript](Scripting-with-AngelScript.md) -- Text-based scripting (the compilation target)
- [Entity-Component-System](Entity-Component-System.md) -- How scripts attach to entities

# Event Response System

## Status: Active (April 2026)

The Event Response System provides a **no-code "When/If/Then" rule engine** for common gameplay patterns. Rules are defined as data (no scripting required) and evaluated at runtime.

### Architecture

| Component | File | Purpose |
|-----------|------|---------|
| `EventResponseSystem` | `SparkEngine/Source/Engine/Gameplay/EventResponseSystem.h` | Rule engine (singleton) |
| `EventResponsePanel` | `SparkEditor/Source/Panels/EventResponsePanel.h` | Visual rule builder |

### Rule Structure

```
WHEN [Event Trigger]
  IF [Conditions] (optional, AND/OR/NOT groups)
THEN [Action 1] → [Action 2] → ...
```

### Trigger Types (16)

OnTriggerEnter, OnTriggerExit, OnDamaged, OnKilled, OnItemPickup, OnKeyPress, OnKeyRelease, OnCollision, OnQuestComplete, OnTimer, OnStart, OnWeatherChange, OnTimeOfDay, OnEntityCreated, OnEntityDestroyed, OnCustom

### Action Types (24)

SpawnEntity, DestroyEntity, EnableEntity, DisableEntity, SetPosition, MoveToward, TeleportEntity, RotateEntity, SetHealth, DealDamage, HealEntity, PlaySound, StopSound, PlayAnimation, ApplyForce, ApplyImpulse, ShowDialogue, ShowMessage, SetWeather, SetTimeOfDay, SetWorldVariable, SetWorldFlag, Delay, FireCustomEvent

### Integration

- Subscribes to existing `EventBus` events via RAII handles
- Conditions reuse `ConditionSystem` (60+ condition types)
- Rules can be serialized to/from JSON
- Timer-based rules and delayed actions processed each frame via `Update(dt)`
- One-shot rules auto-disable after firing once

# Entity Presets

## Status: Active (April 2026)

Pre-configured entity templates for rapid no-code game creation.

### Built-in Presets (12)

| Category | Presets |
|----------|---------|
| **Character** | Player, Enemy, NPC |
| **Prop** | Door, Chest, Destructible |
| **Pickup** | Health Pickup, Ammo Pickup, Key Item |
| **Effect** | Sound Source, Particle Effect, Spawn Point |

### Usage

Right-click in the Hierarchy panel → **Create From Preset** → select category → select preset.

### Architecture

| Component | File | Purpose |
|-----------|------|---------|
| `EntityPresetManager` | `SparkEngine/Source/Engine/ECS/EntityPresetManager.h` | Preset registry (singleton) |
| HierarchyPanel integration | `SparkEditor/Source/Panels/HierarchyPanel.cpp` | Context menu preset picker |

---

# Visual Scripting Deep Dive

## Complete Workflow: Creating a Visual Script

### Step 1: Open the Visual Script Panel

Open the Visual Script panel from the editor menu. The panel is divided into four areas:

- **Left sidebar** -- Node Palette organized by category (Events, Flow Control, Actions, Math, Logic, Getters, Constants, Variables)
- **Center** -- Canvas with pan (middle-mouse), zoom (scroll wheel), node dragging, and bezier connection drawing
- **Right sidebar** -- Variable definitions and selected node properties
- **Bottom bar** -- Compile button, error display, generated script path

### Step 2: Add Nodes

Nodes can be added in two ways:
1. **Click** a node type in the left sidebar palette to place it on the canvas
2. **Right-click** the canvas to open the context menu and select a node type

```cpp
// Programmatic node creation (used by undo/redo system)
panel.AddNodeAtPositionDirect(Spark::Scripting::ScriptNodeType::OnUpdate, 100.0f, 200.0f);
panel.AddNodeAtPositionDirect(Spark::Scripting::ScriptNodeType::GetKeyDown, 300.0f, 200.0f);
panel.AddNodeAtPositionDirect(Spark::Scripting::ScriptNodeType::Branch, 500.0f, 200.0f);
panel.AddNodeAtPositionDirect(Spark::Scripting::ScriptNodeType::ApplyForce, 700.0f, 150.0f);
```

### Step 3: Connect Pins

Drag from an output pin to an input pin to create a connection. The system enforces type safety:

```cpp
// Type compatibility check (from VisualScriptPanel)
// Execution pins only connect to Execution pins
// Data pins connect if types match, or if either pin is PinKind::Any
bool AreTypesCompatible(PinKind a, PinKind b)
{
    if (a == PinKind::Any || b == PinKind::Any) return true;
    return a == b;
}
```

Pin types and their wire colors:

| PinKind | Purpose | Typical Color |
|---------|---------|---------------|
| `Execution` | Flow control (white wire) | White |
| `Bool` | Boolean values | Red |
| `Int` | Integer values | Cyan |
| `Float` | Floating-point values | Green |
| `String` | Text values | Magenta |
| `Vector3` | 3D position/direction | Yellow |
| `Entity` | Entity ID reference | Orange |
| `Any` | Untyped (resolved at compile) | Gray |

### Step 4: Define Variables

In the right sidebar, add variables that become class members in the generated script:

```cpp
// Variable definitions compile to class member declarations
struct VariableDecl
{
    std::string name;                   // Variable name (e.g., "speed")
    PinKind type = PinKind::Float;      // Data type
    std::string defaultValue;           // Initial value (e.g., "10.0")
};

// In the generated AngelScript:
// class MyScript {
//     float speed = 10.0;
//     ...
// }
```

### Step 5: Compile

Click the **Compile** button. The compiler performs:

1. **Find event entry points** -- Scans for `OnStart`, `OnUpdate`, `OnTriggerEnter`, etc.
2. **Topological sort** -- Walks execution and data connections to determine evaluation order
3. **Code emission** -- Generates AngelScript class with member variables and lifecycle methods
4. **Write to disk** -- Saves `.as` file to `Assets/Scripts/Generated/`
5. **Hot reload** -- `ScriptHotReload` detects the file change and recompiles automatically

```cpp
// Programmatic compilation
Spark::Scripting::VisualScriptGraph graph;
graph.className = "PlayerController";
// ... add nodes, connections, variables ...

auto result = Spark::Scripting::VisualScriptCompiler::Compile(graph);
if (result.success)
{
    // result.angelScriptSource contains the generated code
    // Write it to Assets/Scripts/Generated/PlayerController.as
}
else
{
    for (const auto& error : result.errors)
    {
        LOG_ERROR("Compile error: {}", error);
    }
}
```

## Complete Node Type Reference

### Event Nodes (Entry Points)

Event nodes are the starting points of execution chains. Each generates a method in the AngelScript class.

| Node Type | ScriptNodeType Value | Generated Method | Parameters |
|-----------|---------------------|------------------|------------|
| OnStart | 0 | `void Start()` | None |
| OnUpdate | 1 | `void Update(float dt)` | deltaTime |
| OnTriggerEnter | 2 | `void OnTriggerEnter(uint entity)` | triggering entity |
| OnTriggerExit | 3 | `void OnTriggerExit(uint entity)` | exiting entity |
| OnDamaged | 4 | `void OnDamaged(float amount)` | damage amount |
| OnKeyPress | 5 | `void OnKeyPress(string key)` | key name |
| OnCollision | 6 | `void OnCollision(uint entity)` | colliding entity |
| OnCustomEvent | 7 | `void OnCustomEvent(...)` | user-defined |

### Flow Control Nodes

| Node | Behavior |
|------|----------|
| **Branch** | If/else -- routes execution based on a bool input |
| **ForLoop** | Repeats execution N times with an index output |
| **Sequence** | Executes multiple output chains in order |
| **DoNothing** | Pass-through (useful for graph organization) |

### Action Nodes

| Node | Effect |
|------|--------|
| **SetPosition** | Move entity to a Vector3 position |
| **SetRotation** | Set entity rotation |
| **SetHealth** | Set entity health value |
| **ApplyForce** | Apply physics force to entity |
| **PlaySound** | Play an audio clip |
| **PlayAnimation** | Trigger animation state |
| **SpawnEntity** | Create a new entity at position |
| **DestroyEntity** | Remove entity from world |
| **PrintMessage** | Output debug text |
| **FireEvent** | Fire a custom event |

### Math Nodes

| Node | Operation | Inputs | Output |
|------|-----------|--------|--------|
| Add | a + b | Float/Vector3 | Float/Vector3 |
| Subtract | a - b | Float/Vector3 | Float/Vector3 |
| Multiply | a * b | Float | Float |
| Divide | a / b | Float | Float |
| Lerp | Linear interpolate | a, b, t | Float/Vector3 |
| Clamp | Constrain to range | value, min, max | Float |
| Random | Random [0, 1] | None | Float |
| RandomRange | Random [min, max] | min, max | Float |
| Distance | Vector distance | a, b | Float |
| Normalize | Unit vector | Vector3 | Vector3 |
| DotProduct | Dot product | a, b | Float |
| Abs | Absolute value | Float | Float |
| Negate | -value | Float | Float |

## Custom Node Creation Guide

### Defining a Custom Function Sub-Graph

Visual scripts support reusable function sub-graphs that compile to separate methods:

```cpp
// Define a function sub-graph
Spark::Scripting::FunctionGraph healFunction;
healFunction.name = "HealEntity";
healFunction.returnType = Spark::Scripting::PinKind::Execution; // void return

// Add parameters
Spark::Scripting::VariableDecl targetParam;
targetParam.name = "target";
targetParam.type = Spark::Scripting::PinKind::Entity;
healFunction.parameters.push_back(targetParam);

Spark::Scripting::VariableDecl amountParam;
amountParam.name = "amount";
amountParam.type = Spark::Scripting::PinKind::Float;
amountParam.defaultValue = "25.0";
healFunction.parameters.push_back(amountParam);

// Add nodes inside the function (SetHealth, etc.)
// ... add nodes and connections ...

// Register with the graph
graph.functions.push_back(healFunction);
```

This compiles to:

```angelscript
class MyScript
{
    void HealEntity(uint target, float amount)
    {
        setHealth(target, getHealth(target) + amount);
    }
}
```

### Defining Custom Events

```cpp
Spark::Scripting::CustomEventDef onLevelUp;
onLevelUp.name = "OnLevelUp";

Spark::Scripting::VariableDecl levelParam;
levelParam.name = "newLevel";
levelParam.type = Spark::Scripting::PinKind::Int;
onLevelUp.parameters.push_back(levelParam);

graph.customEvents.push_back(onLevelUp);
```

## Debugging Visual Scripts

### Debug Mode Compilation

Enable debug mode to insert `debugTrace()` calls at every node, allowing step-by-step execution tracing:

```cpp
auto result = Spark::Scripting::VisualScriptCompiler::Compile(graph, true); // debugMode = true
```

This inserts trace calls into the generated AngelScript:

```angelscript
void Update(float dt)
{
    debugTrace("Node_1: GetKeyDown");       // Trace each node execution
    bool node_1_0 = getKeyDown("Space");
    debugTrace("Node_2: Branch");
    if (node_1_0)
    {
        debugTrace("Node_3: ApplyForce");
        applyForce(0, 10, 0);
    }
}
```

### Editor Debug Toggle

The `VisualScriptPanel` has a `m_debugCompile` flag that can be toggled in the UI. When enabled, compiled scripts include trace output.

## Undo/Redo Support

All graph operations support undo/redo through the editor command system:

```cpp
// AddNodeCommand -- undoable node creation
class AddNodeCommand : public EditorCommand
{
    void Execute() override;     // Creates the node
    void Undo() override;        // Removes the node
};

// RemoveNodeCommand -- undoable node deletion (saves full node state)
class RemoveNodeCommand : public EditorCommand
{
    void Execute() override;     // Removes node + saves connections
    void Undo() override;        // Restores node + reconnects
};

// AddConnectionCommand -- undoable connection creation
class AddConnectionCommand : public EditorCommand
{
    void Execute() override;     // Creates the connection
    void Undo() override;        // Removes the connection
};
```

## Save/Load Graph Persistence

Graphs can be saved and loaded independently of compilation:

```cpp
// Save graph to JSON
panel.SaveGraph("Assets/Scripts/Graphs/PlayerController.vsgraph");

// Load graph from JSON
panel.LoadGraph("Assets/Scripts/Graphs/PlayerController.vsgraph");
```

## Performance Considerations

1. **No runtime overhead** -- Visual scripts compile to the same AngelScript as hand-written code. There is no visual script interpreter or VM.
2. **Compile-time cost** -- The topological sort and code generation are O(N) where N is the node count. Compilation is near-instant for typical graphs (< 200 nodes).
3. **Hot reload latency** -- After compilation, `ScriptHotReload` detects the file change within one frame and triggers AngelScript recompilation. Total turnaround is typically under 100ms.
4. **Graph size** -- Large graphs (500+ nodes) may slow canvas rendering in the editor. Consider splitting logic across multiple scripts attached to different entities.

## Integration with AngelScript

The generated code uses the standard AngelScript API bindings:

```angelscript
// All visual script outputs use the same engine bindings as hand-written scripts
class PlayerController
{
    float speed = 5.0;
    float jumpForce = 10.0;

    void Start()
    {
        print("PlayerController initialized");
    }

    void Update(float dt)
    {
        if (getKeyDown("W"))
        {
            vec3 pos = getPosition(getSelf());
            pos.y += speed * dt;
            setPosition(getSelf(), pos);
        }
        if (getKeyDown("Space"))
        {
            applyForce(getSelf(), 0, jumpForce, 0);
        }
    }
}
```

The compiler's `PinTypeString()` maps each `PinKind` to its AngelScript equivalent:

| PinKind | AngelScript Type |
|---------|-----------------|
| `Bool` | `bool` |
| `Int` | `int` |
| `Float` | `float` |
| `String` | `string` |
| `Vector3` | `vec3` |
| `Entity` | `uint` |
| `Execution` | (flow control, no type) |
