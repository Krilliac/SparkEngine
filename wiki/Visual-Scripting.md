# Visual Scripting

## Overview

SparkEngine includes a node-based visual scripting system that compiles to AngelScript. Designers can create gameplay logic using a graph editor without writing code.

## Architecture

- **Namespace:** `Spark::Scripting`
- **Files:** `Engine/Scripting/VisualScriptSystem.h/.cpp`
- **Dependencies:** AngelScript (optional runtime compilation)

## Key Classes

| Class | Purpose |
|-------|---------|
| `VisualScriptSystem` | Top-level singleton manager |
| `VisualScriptGraph` | A directed graph of connected nodes |
| `ScriptNode` | A single node with typed input/output pins |
| `NodeLibrary` | Registry of built-in and custom node templates |

## Pin Types

| Type | Colour | Description |
|------|--------|-------------|
| Execution | White | Controls flow — which node fires next |
| Bool | Red | Boolean values |
| Int | Teal | 32-bit integers |
| Float | Green | Floating-point numbers |
| String | Magenta | Text strings |
| Vector3 | Gold | 3D vectors |
| Entity | Blue | Entity ID references |

## Built-in Nodes

### Events
- **BeginPlay** — fires once when the script starts
- **Tick** — fires every frame with delta time
- **OnCollision** — fires on physics collision

### Flow Control
- **Branch** — if/else
- **ForLoop** — counted loop
- **Sequence** — execute multiple branches in order
- **DoOnce** — execute only the first time

### Math
Add, Subtract, Multiply, Divide, Clamp, Lerp, Abs, Sin, Cos, Sqrt, Min, Max

### Entity
GetPosition, SetPosition, SpawnEntity, DestroyEntity

### Debug
PrintString, DrawDebugLine

## Compilation Pipeline

1. Graph is validated (type checks, cycle detection, unreachable node warnings)
2. Topological sort determines execution order
3. Each node emits AngelScript code via its `codeTemplate`
4. Variables become class members, events become methods
5. Output is a complete AngelScript class ready for `CompileScriptFromString()`

## Usage

```cpp
auto& system = VisualScriptSystem::GetInstance();
system.Initialize();

auto* graph = system.CreateGraph("PlayerController");
NodeID beginPlay = graph->AddNode("Event_BeginPlay");
NodeID print = graph->AddNode("Debug_Print");
// Connect execution pins...

std::string asSource = graph->CompileToAngelScript();
system.CompileAndRegister("PlayerController");
```

## Serialization

Graphs serialize to JSON for editor persistence:
```cpp
std::string json = graph->SerializeToJSON();
graph->DeserializeFromJSON(json);
```

## Testing

8 unit tests in `Tests/TestVisualScriptSystem.cpp` covering node management, link management, validation, variables, compilation, serialization, and system lifecycle.
