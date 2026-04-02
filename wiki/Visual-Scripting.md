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
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Text-based scripting (the compilation target)
- [Entity-Component-System](Entity-Component-System) -- How scripts attach to entities

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
