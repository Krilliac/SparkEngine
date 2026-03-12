# Visual Scripting

## Overview

SparkEngine includes a node-based visual scripting system that compiles to AngelScript. Designers can create gameplay logic using a graph editor without writing code. The system supports type-safe pins, execution and data flow separation, a comprehensive built-in node library, hot-reload during play mode, and JSON serialization for editor persistence.

**Source:** `SparkEngine/Source/Engine/Scripting/VisualScriptSystem.h`, `VisualScriptSystem.cpp`

**Namespace:** `Spark::Scripting`

**Dependencies:** AngelScript (optional runtime compilation)

---

## Architecture

```
+------------------------------------------------------------+
|                   VisualScriptSystem                        |
|  (Singleton -- top-level manager)                           |
|                                                             |
|  m_graphs: map<string, unique_ptr<VisualScriptGraph>>      |
|  m_isInitialized: bool                                      |
|                                                             |
|  Initialize() / Shutdown()                                  |
|  CreateGraph() / GetGraph() / RemoveGraph()                 |
|  CompileAndRegister() / HotReload()                         |
|  SaveGraph() / LoadGraph()                                  |
+----------------------------+-------------------------------+
                             |
              +--------------+--------------+
              |     VisualScriptGraph        |
              |  (directed graph of nodes)   |
              |                              |
              |  m_nodes: map<NodeID, Node>  |
              |  m_links: vector<LinkDesc>   |
              |  m_variables: vector<Var>    |
              |                              |
              |  AddNode() / RemoveNode()    |
              |  AddLink() / RemoveLink()    |
              |  Validate()                  |
              |  CompileToAngelScript()      |
              |  Execute() (interpreter)     |
              |  SerializeToJSON()           |
              +---+-----------+------+------+
                  |           |      |
       +----------+   +------+  +---+--------+
       | ScriptNode|   |LinkDesc|  |GraphVar  |
       | (node)    |   |(wire)  |  |(variable)|
       +-----------+   +--------+  +----------+

+------------------------------------------------------------+
|                     NodeLibrary                             |
|  (Singleton -- registry of node templates)                  |
|                                                             |
|  m_templates: map<string, NodeTemplate>                     |
|  RegisterBuiltinNodes() / RegisterTemplate()                |
|  FindTemplate() / GetTemplatesByCategory()                  |
+------------------------------------------------------------+
```

## Key Classes

| Class | Purpose |
|-------|---------|
| `VisualScriptSystem` | Top-level singleton manager. Owns all graphs, handles compilation, hot-reload, and disk I/O. |
| `VisualScriptGraph` | A complete directed graph of `ScriptNode` instances connected by `LinkDesc` wires. Supports validation, compilation to AngelScript, interpreter-mode execution, and JSON serialization. |
| `ScriptNode` | A single node with typed input/output pins, an execute callback for interpreter mode, a code template for compilation, and an editor position. |
| `NodeLibrary` | Singleton registry of `NodeTemplate` structures. Provides all built-in nodes and supports runtime registration of custom nodes. |

---

## ID Types and Constants

| Type | Underlying | Description |
|------|-----------|-------------|
| `NodeID` | `uint32_t` | Unique identifier for a node within a graph |
| `PinID` | `uint32_t` | Unique identifier for a pin within a node |
| `LinkID` | `uint32_t` | Unique identifier for a link within a graph |

| Constant | Value | Meaning |
|----------|-------|---------|
| `INVALID_NODE_ID` | `0` | Sentinel for uninitialized or invalid node references |
| `INVALID_PIN_ID` | `0` | Sentinel for uninitialized or invalid pin references |
| `INVALID_LINK_ID` | `0` | Sentinel for uninitialized or invalid link references |

---

## Pin Types

The `PinType` enum defines all data types that pins can carry:

```cpp
enum class PinType : uint8_t
{
    Execution,  // White wire -- controls which node fires next
    Bool,       // Boolean values
    Int,        // 32-bit integers
    Float,      // Floating-point numbers
    String,     // Text strings
    Vector2,    // 2D vectors (array<float, 2>)
    Vector3,    // 3D vectors (array<float, 3>)
    Vector4,    // 4D vectors (array<float, 4>)
    Entity,     // Entity ID reference (uint64_t)
    Any         // Wildcard -- resolved at connection time
};
```

| Type | Editor Colour | PinValue Variant | AngelScript Type |
|------|--------------|------------------|------------------|
| `Execution` | White | N/A (flow only) | `void` |
| `Bool` | Red | `bool` | `bool` |
| `Int` | Teal | `int32_t` | `int` |
| `Float` | Green | `float` | `float` |
| `String` | Magenta | `std::string` | `string` |
| `Vector2` | Yellow | `std::array<float, 2>` | `Vector2` |
| `Vector3` | Gold | `std::array<float, 3>` | `Vector3` |
| `Vector4` | Orange | `std::array<float, 4>` | `Vector4` |
| `Entity` | Blue | `uint64_t` | `uint64` |
| `Any` | Grey | (resolved at connection) | (depends on resolution) |

### Pin Direction

```cpp
enum class PinDirection : uint8_t
{
    Input,
    Output
};
```

### PinValue Variant

Pin values are stored as a `std::variant`:

```cpp
using PinValue = std::variant<
    std::monostate,          // No value (unset)
    bool,                    // Bool
    int32_t,                 // Int
    float,                   // Float
    std::string,             // String
    std::array<float, 2>,    // Vector2
    std::array<float, 3>,    // Vector3
    std::array<float, 4>,    // Vector4
    uint64_t                 // Entity ID
>;
```

---

## Pin and Link Descriptors

### PinDesc

```cpp
struct PinDesc
{
    PinID id = INVALID_PIN_ID;
    std::string name;
    PinType type = PinType::Any;
    PinDirection direction = PinDirection::Input;
    PinValue defaultValue;
};
```

### LinkDesc

```cpp
struct LinkDesc
{
    LinkID id = INVALID_LINK_ID;
    NodeID sourceNode = INVALID_NODE_ID;
    PinID sourcePin = INVALID_PIN_ID;
    NodeID targetNode = INVALID_NODE_ID;
    PinID targetPin = INVALID_PIN_ID;
};
```

---

## Node Categories

```cpp
enum class NodeCategory : uint8_t
{
    Event,        // BeginPlay, Tick, OnCollision
    FlowControl,  // Branch, ForLoop, Sequence, DoOnce
    Math,         // Add, Multiply, Clamp, Lerp, etc.
    Logic,        // AND, OR, NOT, Compare
    String,       // Concat, Format, Length, Substring
    Variable,     // Get/Set local or graph variables
    Entity,       // GetPosition, SpawnEntity, Destroy
    Physics,      // AddForce, Raycast, SetVelocity
    Input,        // IsKeyDown, GetAxis, GetMousePos
    Audio,        // PlaySound, StopSound, SetVolume
    Debug,        // PrintString, DrawLine, Log
    Custom        // User-registered nodes
};
```

---

## ScriptNode API

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `ScriptNode(NodeID id, const std::string& name, NodeCategory category)` | Create a node with the given ID, type name, and category. |
| `GetID` | `NodeID GetID() const` | Return the unique node ID. |
| `GetName` | `const std::string& GetName() const` | Return the node type name (e.g. `"Branch"`). |
| `GetCategory` | `NodeCategory GetCategory() const` | Return the node category. |
| `AddInputPin` | `PinID AddInputPin(const std::string& name, PinType type, const PinValue& defaultVal = {})` | Add a typed input pin with optional default value. Returns the assigned `PinID`. |
| `AddOutputPin` | `PinID AddOutputPin(const std::string& name, PinType type)` | Add a typed output pin. Returns the assigned `PinID`. |
| `GetInputPins` | `const std::vector<PinDesc>& GetInputPins() const` | Return all input pin descriptors. |
| `GetOutputPins` | `const std::vector<PinDesc>& GetOutputPins() const` | Return all output pin descriptors. |
| `FindPin` | `const PinDesc* FindPin(PinID pinId) const` | Look up a pin by ID across both input and output pins. Returns `nullptr` if not found. |
| `SetExecuteCallback` | `void SetExecuteCallback(ExecuteCallback cb)` | Set the runtime execution callback for interpreter mode. |
| `GetExecuteCallback` | `const ExecuteCallback& GetExecuteCallback() const` | Get the execution callback. |
| `SetCodeTemplate` | `void SetCodeTemplate(const std::string& code)` | Set the AngelScript code template for compilation. |
| `GetCodeTemplate` | `const std::string& GetCodeTemplate() const` | Get the code template. |
| `SetPosition` | `void SetPosition(float x, float y)` | Set the editor canvas position (serialization only). |
| `GetPosX` / `GetPosY` | `float GetPosX() const` / `float GetPosY() const` | Get the editor canvas position. |

The `ExecuteCallback` type is:

```cpp
using ExecuteCallback = std::function<std::vector<PinValue>(const std::vector<PinValue>&)>;
```

It receives resolved input values and returns output values. Used in interpreter mode.

---

## Graph Variables

```cpp
struct GraphVariable
{
    std::string name;
    PinType type = PinType::Float;
    PinValue value;
    bool isPublic = false;  // Exposed to the editor / inspector
};
```

Graph variables become class member variables in the compiled AngelScript output. Public variables are editable in the entity inspector when the script is attached to an entity.

---

## VisualScriptGraph API

### Node Management

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddNode` | `NodeID AddNode(const std::string& typeName, float posX = 0, float posY = 0)` | Create a node from a `NodeLibrary` template. Returns `INVALID_NODE_ID` if the template is not found. |
| `RemoveNode` | `bool RemoveNode(NodeID id)` | Remove a node and all its connected links. |
| `GetNode` | `ScriptNode* GetNode(NodeID id)` | Get a node by ID (mutable). Returns `nullptr` if not found. |
| `GetNodes` | `const std::unordered_map<NodeID, ScriptNode>& GetNodes() const` | Return all nodes in the graph. |

### Link Management

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddLink` | `LinkID AddLink(NodeID srcNode, PinID srcPin, NodeID dstNode, PinID dstPin)` | Create a connection between pins. Validates type compatibility and checks for cycles. Any existing link to the destination input pin is removed (inputs accept one connection). Returns `INVALID_LINK_ID` on failure. |
| `RemoveLink` | `bool RemoveLink(LinkID id)` | Remove a link by ID. |
| `GetLinks` | `const std::vector<LinkDesc>& GetLinks() const` | Return all links. |
| `CanConnect` | `bool CanConnect(NodeID srcNode, PinID srcPin, NodeID dstNode, PinID dstPin) const` | Check if a connection is valid (type compatibility, direction checks, cycle detection). |

### Connection Validation Rules (CanConnect)

1. Source and destination must be different nodes (no self-connections).
2. Both nodes and pins must exist.
3. Source pin must be an output; destination pin must be an input.
4. Type compatibility: Execution pins only connect to Execution pins. Data pins connect if types match or either is `PinType::Any`.
5. No cycles: adding the link must not create a cycle in the graph (checked via DFS from the destination back to the source).

### Variable Management

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddVariable` | `void AddVariable(const std::string& name, PinType type, const PinValue& defaultVal = {}, bool isPublic = false)` | Add a variable. Overwrites if a variable with the same name already exists. |
| `RemoveVariable` | `bool RemoveVariable(const std::string& name)` | Remove a variable by name. |
| `GetVariable` | `GraphVariable* GetVariable(const std::string& name)` | Get a variable by name. Returns `nullptr` if not found. |
| `GetVariables` | `const std::vector<GraphVariable>& GetVariables() const` | Return all variables. |

### Validation

```cpp
struct ValidationResult
{
    bool isValid = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

ValidationResult Validate() const;
```

Validation checks performed:

1. **Empty graph warning:** If the graph has no nodes, a warning is emitted.
2. **No event nodes warning:** If no nodes have `NodeCategory::Event`, a warning is emitted (nothing will trigger execution).
3. **Unconnected input pins:** Data input pins that are not connected and have no default value produce warnings.
4. **Invalid link references:** Links referencing missing nodes or pins produce errors.
5. **Type mismatches:** Links connecting incompatible pin types (neither matching nor `Any`) produce errors.
6. **Unreachable nodes:** Nodes not reachable from any event node via the link graph produce warnings.

### Compilation

| Method | Signature | Description |
|--------|-----------|-------------|
| `CompileToAngelScript` | `std::string CompileToAngelScript() const` | Compile the graph to AngelScript source code. Returns empty string on failure. |

### Interpreter Mode Execution

| Method | Signature | Description |
|--------|-----------|-------------|
| `Execute` | `void Execute(NodeID eventNodeId)` | Execute the graph starting from the specified event node, using node `ExecuteCallback` functions. |

### Serialization

| Method | Signature | Description |
|--------|-----------|-------------|
| `SerializeToJSON` | `std::string SerializeToJSON() const` | Serialize the graph to a JSON string. |
| `DeserializeFromJSON` | `bool DeserializeFromJSON(const std::string& json)` | Deserialize from JSON. Clears existing graph state. Returns `false` if parsing or validation fails. |

---

## Built-in Node Library (Complete Reference)

The `NodeLibrary::RegisterBuiltinNodes()` method registers all built-in nodes. The following tables document every registered node from the source code.

### Event Nodes

| Type Name | Display Name | Output Pins | Code Template |
|-----------|-------------|-------------|---------------|
| `BeginPlay` | Begin Play | Then (Exec) | (method: `BeginPlay()`) |
| `Tick` | Tick | Then (Exec), DeltaTime (Float) | (method: `Tick(float deltaTime)`) |
| `OnCollision` | On Collision | Then (Exec), OtherEntity (Entity) | (method: `OnCollision(uint64 otherEntity)`) |

### Flow Control Nodes

| Type Name | Display Name | Input Pins | Output Pins | Code Template |
|-----------|-------------|------------|-------------|---------------|
| `Branch` | Branch | Exec, Condition (Bool, default: false) | True (Exec), False (Exec) | `if ({Condition})` |
| `ForLoop` | For Loop | Exec, Start (Int, default: 0), End (Int, default: 10) | LoopBody (Exec), Completed (Exec), Index (Int) | `for (int i = {Start}; i < {End}; i++)` |
| `Sequence` | Sequence | Exec | Then0 (Exec), Then1 (Exec), Then2 (Exec) | (none -- sequential dispatch) |
| `DoOnce` | Do Once | Exec | Then (Exec) | `if (!_doOnce) { _doOnce = true;` |

### Math Nodes

| Type Name | Display Name | Input Pins | Output | Code Template |
|-----------|-------------|------------|--------|---------------|
| `Add` | Add | A (Float, 0.0), B (Float, 0.0) | Result (Float) | `({A} + {B})` |
| `Subtract` | Subtract | A (Float, 0.0), B (Float, 0.0) | Result (Float) | `({A} - {B})` |
| `Multiply` | Multiply | A (Float, 0.0), B (Float, 0.0) | Result (Float) | `({A} * {B})` |
| `Divide` | Divide | A (Float, 0.0), B (Float, 0.0) | Result (Float) | `({A} / {B})` (with zero-check in callback) |
| `Clamp` | Clamp | Value (Float, 0.0), Min (Float, 0.0), Max (Float, 1.0) | Result (Float) | `Math::Clamp({Value}, {Min}, {Max})` |
| `Lerp` | Lerp | A (Float, 0.0), B (Float, 1.0), Alpha (Float, 0.5) | Result (Float) | `Math::Lerp({A}, {B}, {Alpha})` |
| `Abs` | Abs | Value (Float, 0.0) | Result (Float) | `Math::Abs({Value})` |
| `Sin` | Sin | Value (Float, 0.0) | Result (Float) | `Math::Sin({Value})` |
| `Cos` | Cos | Value (Float, 0.0) | Result (Float) | `Math::Cos({Value})` |
| `Sqrt` | Square Root | Value (Float, 0.0) | Result (Float) | `Math::Sqrt({Value})` |
| `Min` | Min | A (Float, 0.0), B (Float, 0.0) | Result (Float) | `Math::Min({A}, {B})` |
| `Max` | Max | A (Float, 0.0), B (Float, 0.0) | Result (Float) | `Math::Max({A}, {B})` |

### Logic Nodes

| Type Name | Display Name | Input Pins | Output | Code Template |
|-----------|-------------|------------|--------|---------------|
| `AND` | AND | A (Bool, false), B (Bool, false) | Result (Bool) | `({A} && {B})` |
| `OR` | OR | A (Bool, false), B (Bool, false) | Result (Bool) | `({A} \|\| {B})` |
| `NOT` | NOT | Value (Bool, false) | Result (Bool) | `(!{Value})` |
| `Equal` | Equal | A (Float, 0.0), B (Float, 0.0) | Result (Bool) | `({A} == {B})` |
| `NotEqual` | Not Equal | A (Float, 0.0), B (Float, 0.0) | Result (Bool) | `({A} != {B})` |
| `Greater` | Greater Than | A (Float, 0.0), B (Float, 0.0) | Result (Bool) | `({A} > {B})` |
| `Less` | Less Than | A (Float, 0.0), B (Float, 0.0) | Result (Bool) | `({A} < {B})` |

### String Nodes

| Type Name | Display Name | Input Pins | Output | Code Template |
|-----------|-------------|------------|--------|---------------|
| `Concat` | Concatenate | A (String, ""), B (String, "") | Result (String) | `({A} + {B})` |
| `Length` | String Length | Value (String, "") | Length (Int) | `{Value}.length()` |
| `Substring` | Substring | Value (String, ""), Start (Int, 0), Count (Int, 1) | Result (String) | `{Value}.substr({Start}, {Count})` |
| `ToString` | To String | Value (Float, 0.0) | Result (String) | `formatFloat({Value})` |

### Variable Nodes

| Type Name | Display Name | Input Pins | Output Pins | Code Template |
|-----------|-------------|------------|-------------|---------------|
| `GetVariable` | Get Variable | Name (String, "") | Value (Any) | `{Name}` |
| `SetVariable` | Set Variable | Exec, Name (String, ""), Value (Any) | Then (Exec) | `{Name} = {Value};` |

### Entity Nodes

| Type Name | Display Name | Input Pins | Output Pins | Code Template |
|-----------|-------------|------------|-------------|---------------|
| `GetPosition` | Get Position | Entity (Entity) | Position (Vector3) | `GetEntityPosition({Entity})` |
| `SetPosition` | Set Position | Exec, Entity (Entity), Position (Vector3) | Then (Exec) | `SetEntityPosition({Entity}, {Position});` |
| `SpawnEntity` | Spawn Entity | Exec, Template (String, "") | Then (Exec), Entity (Entity) | `uint64 {Entity} = SpawnEntity({Template});` |
| `DestroyEntity` | Destroy Entity | Exec, Entity (Entity) | Then (Exec) | `DestroyEntity({Entity});` |

### Input Nodes

| Type Name | Display Name | Input Pins | Output | Code Template |
|-----------|-------------|------------|--------|---------------|
| `IsKeyDown` | Is Key Down | Key (String, "Space") | Pressed (Bool) | `Input::IsKeyDown({Key})` |
| `GetAxis` | Get Axis | Axis (String, "Horizontal") | Value (Float) | `Input::GetAxis({Axis})` |

### Debug Nodes

| Type Name | Display Name | Input Pins | Output Pins | Code Template |
|-----------|-------------|------------|-------------|---------------|
| `PrintString` | Print String | Exec, Message (String, "Hello") | Then (Exec) | `Print({Message});` |
| `DrawDebugLine` | Draw Debug Line | Exec, Start (Vector3), End (Vector3), Duration (Float, 1.0) | Then (Exec) | `Debug::DrawLine({Start}, {End}, {Duration});` |

---

## Compilation Pipeline In Depth

The `CompileToAngelScript()` method transforms a visual graph into executable AngelScript source code.

### Step 1: Class Declaration

The graph name is sanitized to a valid identifier (only `[a-zA-Z_0-9]` characters, cannot start with a digit). The output begins with:

```angelscript
// Auto-generated from visual script: PlayerController
class PlayerController
{
```

### Step 2: Member Variables

All graph variables are emitted as class members with their AngelScript type and optional default value:

```angelscript
    float moveSpeed = 5.0f;
    bool isAlive = true;
    Vector3 spawnPos;
```

### Step 3: Event Methods

For each node with `NodeCategory::Event`, a method is generated. The method name matches the node name. Special parameters are added based on the event type:

| Event | Method Signature |
|-------|-----------------|
| `BeginPlay` | `void BeginPlay()` |
| `Tick` | `void Tick(float deltaTime)` |
| `OnCollision` | `void OnCollision(uint64 otherEntity)` |

### Step 4: Topological Sort and Data Flow

The graph is topologically sorted so that every node is emitted after all its data dependencies. For each event method:

1. All nodes reachable from the event are identified.
2. Output pins of data nodes are assigned temporary variable names (`temp_0`, `temp_1`, etc.).
3. Link connections propagate variable names from source output pins to target input pins.

### Step 5: Code Emission

The code generator walks the execution flow from the event node using a recursive `emitExecChain` function:

1. For each node in the execution chain, data dependencies are emitted first (as local variable declarations).
2. The node's code template is emitted with pin placeholders (`{PinName}`) replaced by actual variable names or default value literals.
3. Execution output pins are followed to the next node in the chain.

### Example Output

```angelscript
// Auto-generated from visual script: PlayerController
class PlayerController
{
    float moveSpeed = 5.0f;

    void BeginPlay()
    {
        Print("Hello");
    }

    void Tick(float deltaTime)
    {
        float temp_0 = (moveSpeed * deltaTime);
        Print(formatFloat(temp_0));
    }
}
```

### Code Template Placeholder Substitution

The `GenerateNodeCode()` method replaces `{PinName}` placeholders in code templates:

- If the pin has an incoming link, the placeholder is replaced with the source variable name.
- If the pin has no link but has a default value, the placeholder is replaced with the AngelScript literal representation of that value.
- Default literal conversions: `bool` -> `"true"/"false"`, `int32_t` -> `"42"`, `float` -> `"3.14f"`, `std::string` -> `"\"hello\""` (with character escaping for `\`, `"`, `\n`, `\r`, `\t`, `\0`).

---

## VisualScriptSystem API

### Lifecycle

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetInstance` | `static VisualScriptSystem& GetInstance()` | Return the singleton instance. |
| `Initialize` | `bool Initialize()` | Initialize the system and register built-in nodes via `NodeLibrary::RegisterBuiltinNodes()`. Safe to call multiple times (idempotent). |
| `Shutdown` | `void Shutdown()` | Clear all graphs and reset initialization state. |

### Graph Management

| Method | Signature | Description |
|--------|-----------|-------------|
| `CreateGraph` | `VisualScriptGraph* CreateGraph(const std::string& name)` | Create a new graph with the given name. If a graph with that name already exists, returns the existing one. |
| `GetGraph` | `VisualScriptGraph* GetGraph(const std::string& name)` | Get a graph by name. Returns `nullptr` if not found. |
| `RemoveGraph` | `bool RemoveGraph(const std::string& name)` | Remove and destroy a graph. |
| `GetGraphs` | `const std::unordered_map<std::string, std::unique_ptr<VisualScriptGraph>>& GetGraphs() const` | Return all graphs. |

### Compilation and Hot-Reload

| Method | Signature | Description |
|--------|-----------|-------------|
| `CompileAndRegister` | `bool CompileAndRegister(const std::string& graphName)` | Validate, compile, and register a graph with the AngelScript engine. Logs errors and warnings. Returns `false` on validation or compilation failure. |
| `HotReload` | `bool HotReload(const std::string& graphName)` | Re-compile and re-register a graph (calls `CompileAndRegister` internally). |

### Disk I/O

| Method | Signature | Description |
|--------|-----------|-------------|
| `SaveGraph` | `bool SaveGraph(const std::string& graphName, const std::string& filePath) const` | Serialize a graph to JSON and write to a file. |
| `LoadGraph` | `bool LoadGraph(const std::string& filePath)` | Read a JSON file, deserialize into a new graph, and add it to the system. The graph name is read from the JSON. |

### Console Integration

| Method | Signature | Description |
|--------|-----------|-------------|
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Return status: initialized state, graph count, template count. |
| `Console_ListGraphs` | `std::string Console_ListGraphs() const` | Return a list of all graphs with their node and link counts. |
| `Console_ListNodes` | `std::string Console_ListNodes() const` | Return all available node types grouped by category. |

---

## NodeLibrary API

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetInstance` | `static NodeLibrary& GetInstance()` | Return the singleton instance. |
| `RegisterTemplate` | `void RegisterTemplate(const NodeTemplate& tmpl)` | Register a node template. Overwrites if one with the same `typeName` exists. |
| `FindTemplate` | `const NodeTemplate* FindTemplate(const std::string& typeName) const` | Look up a template by type name. Returns `nullptr` if not found. |
| `GetTemplatesByCategory` | `std::vector<const NodeTemplate*> GetTemplatesByCategory(NodeCategory category) const` | Return all templates in a given category. |
| `GetAllTemplates` | `const std::unordered_map<std::string, NodeTemplate>& GetAllTemplates() const` | Return the full template map. |
| `RegisterBuiltinNodes` | `void RegisterBuiltinNodes()` | Register all built-in nodes. Idempotent (tracks `m_builtinsRegistered` flag). |

### NodeTemplate Structure

```cpp
struct NodeTemplate
{
    std::string typeName;      // Unique key (e.g. "Branch")
    std::string displayName;   // Human-readable (e.g. "Branch")
    NodeCategory category;
    std::vector<PinDesc> inputPins;
    std::vector<PinDesc> outputPins;
    std::string codeTemplate;
    ScriptNode::ExecuteCallback executeCallback;
};
```

---

## Interpreter Mode Execution

The `VisualScriptGraph::Execute(NodeID eventNodeId)` method provides an interpreter mode that runs the graph without compiling to AngelScript. This is useful for testing and debugging in the editor.

Execution flow:

1. Verify the starting node is an event node.
2. Walk execution output pins recursively.
3. For each node, resolve input values by recursively executing connected data source nodes.
4. Call the node's `ExecuteCallback` with resolved inputs.
5. Follow execution output pins to the next node.
6. A visited set prevents infinite loops (each node executes at most once per invocation).

---

## Serialization Format

The JSON format stores nodes, links, and variables:

```json
{
    "name": "PlayerController",
    "nodes": [
        {
            "id": 1,
            "typeName": "BeginPlay",
            "posX": 100.0,
            "posY": 200.0
        }
    ],
    "links": [
        {
            "id": 1,
            "sourceNode": 1,
            "sourcePin": 2,
            "targetNode": 3,
            "targetPin": 1
        }
    ],
    "variables": [
        {
            "name": "moveSpeed",
            "type": "Float",
            "isPublic": true
        }
    ]
}
```

### Deserialization Behavior

- The graph is cleared before deserialization.
- Nodes are recreated from their `typeName` using the `NodeLibrary`. If a template is not found, the node is skipped.
- Links are inserted directly (validation skipped for performance) but the graph is validated after all links are loaded.
- Variables are recreated with their stored type and public flag.
- `DeserializeFromJSON` returns `false` if the graph fails post-deserialization validation.

---

## Custom Node Registration

Developers can register custom node types:

```cpp
NodeLibrary::NodeTemplate tmpl;
tmpl.typeName = "Custom_Heal";
tmpl.displayName = "Heal Entity";
tmpl.category = NodeCategory::Custom;
tmpl.inputPins = {
    {0, "Exec", PinType::Execution, PinDirection::Input, {}},
    {0, "Entity", PinType::Entity, PinDirection::Input, {}},
    {0, "Amount", PinType::Float, PinDirection::Input, PinValue{50.0f}}
};
tmpl.outputPins = {
    {0, "Then", PinType::Execution, PinDirection::Output, {}}
};
tmpl.codeTemplate = "HealEntity({Entity}, {Amount});";
tmpl.executeCallback = nullptr;  // or provide interpreter callback

NodeLibrary::GetInstance().RegisterTemplate(tmpl);
```

---

## Complete Usage Example

```cpp
#include "Engine/Scripting/VisualScriptSystem.h"
using namespace Spark::Scripting;

void SetupPlayerScript()
{
    // Initialize the system
    auto& system = VisualScriptSystem::GetInstance();
    system.Initialize();

    // Create a new graph
    auto* graph = system.CreateGraph("PlayerController");

    // Add a variable
    graph->AddVariable("moveSpeed", PinType::Float, PinValue{5.0f}, true);

    // Add nodes
    NodeID beginPlay = graph->AddNode("BeginPlay", 100, 200);
    NodeID print = graph->AddNode("PrintString", 400, 200);

    // Connect execution pins
    // Get the output pin IDs from the nodes
    auto* bpNode = graph->GetNode(beginPlay);
    auto* prNode = graph->GetNode(print);
    PinID bpExecOut = bpNode->GetOutputPins()[0].id;  // "Then"
    PinID prExecIn = prNode->GetInputPins()[0].id;     // "Exec"

    graph->AddLink(beginPlay, bpExecOut, print, prExecIn);

    // Validate
    auto result = graph->Validate();
    if (!result.isValid)
    {
        for (const auto& err : result.errors)
            std::cerr << "Error: " << err << std::endl;
        return;
    }

    // Compile to AngelScript
    std::string source = graph->CompileToAngelScript();

    // Or use the system to compile and register
    system.CompileAndRegister("PlayerController");

    // Save to disk
    system.SaveGraph("PlayerController", "Data/Scripts/PlayerController.vsgraph");

    // Later, load from disk
    system.LoadGraph("Data/Scripts/PlayerController.vsgraph");
}
```

---

## Console Commands

| Command | Description | Calls |
|---------|-------------|-------|
| `vs_status` | Show system status (initialized, graph count, template count) | `Console_GetStatus()` |
| `vs_graphs` | List all loaded graphs with node/link counts | `Console_ListGraphs()` |
| `vs_nodes` | List all available node types by category | `Console_ListNodes()` |

---

## Performance Considerations

- **Compilation cost:** Graph compilation (validation + code generation) is typically under 5ms for graphs with fewer than 200 nodes.
- **Runtime cost:** Compiled visual scripts run at AngelScript speed. For performance-critical logic, consider moving hot paths to native C++.
- **Interpreter mode:** Significantly slower than compiled mode due to per-node function call overhead. Use only for editor debugging.
- **Memory:** Each graph stores its nodes, links, and variables. A 100-node graph typically uses 10-50 KB.
- **Hot-reload latency:** Re-compilation and re-registration typically completes in under 50ms.

---

## Troubleshooting

### AddNode returns INVALID_NODE_ID

The node template was not found in the `NodeLibrary`. Ensure the system is initialized (`VisualScriptSystem::Initialize()` calls `RegisterBuiltinNodes()`) and the type name matches exactly (case-sensitive).

### CompileAndRegister returns false

Check the log output for validation errors. Common causes: type mismatches on links, missing node templates after deserialization, or links referencing deleted nodes.

### Deserialization fails

`DeserializeFromJSON` returns `false` if the JSON is malformed or if post-deserialization validation finds errors (e.g. links referencing nodes that could not be recreated because their template is missing).

### Cycle detection prevents linking

The `CanConnect` method checks for cycles by searching for an existing path from the destination node back to the source node. If your graph has a legitimate need for feedback loops, restructure using `ForLoop` or state variables instead.

---

## Testing

8 unit tests in `Tests/TestVisualScriptSystem.cpp` covering:

- Node management (add, remove, get)
- Link management (add, remove, connection validation)
- Cycle detection
- Variable management (add, remove, overwrite)
- Graph validation (errors and warnings)
- Compilation to AngelScript
- JSON serialization and deserialization
- System lifecycle (initialize, shutdown, idempotency)

---

## See Also

- [Scripting with AngelScript](Scripting-with-AngelScript) -- Text-based scripting that visual scripts compile to
- [Entity-Component-System](Entity-Component-System) -- How scripts attach to entities via ScriptComponent
- [SparkEditor](SparkEditor) -- Editor panels including the Visual Script Editor
- [Event System](Event-System) -- Custom events that can trigger visual script event nodes
- [AI System](AI-System) -- Behavior trees can invoke visual scripts and vice versa
- [Physics](Physics) -- Physics nodes for raycasting and force application
- [Input System](Input-System) -- Input nodes for keyboard and gamepad queries
- [Audio System](Audio-System) -- Audio nodes for sound playback control
- [Testing](Testing) -- Unit tests for the visual scripting system
