# Shader Graph

SparkEngine includes a node-based shader graph compiler that translates visual material graphs into HLSL shader code. Artists and technical designers build materials by connecting nodes in the editor, and the compiler generates optimized vertex and pixel shaders.

**Source:** `SparkEngine/Source/Graphics/ShaderGraph/ShaderGraphCompiler.h`
**Namespace:** `Spark::Graphics`
**Tests:** `Tests/TestShaderGraphCompiler.cpp` (5 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Node Types](#node-types)
  - [Input Nodes](#input-nodes)
  - [Math Nodes](#math-nodes)
  - [Utility Nodes](#utility-nodes)
  - [Output Nodes](#output-nodes)
- [Graph Structure](#graph-structure)
  - [ShaderNode](#shadernode)
  - [ShaderConnection](#shaderconnection)
  - [ShaderGraphInput](#shadergraphinput)
  - [ShaderGraphOutput](#shadergraphoutput)
- [Compilation Algorithm](#compilation-algorithm)
- [Usage Example](#usage-example)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

The shader graph system bridges the gap between visual material editing and GPU shader code. The editor (see [SparkEditor](SparkEditor)) provides a node canvas where users connect input, math, and utility nodes to output nodes. The `ShaderGraphCompiler` traverses this graph and emits valid HLSL.

```
┌─────────────────────────────────────────────────────────┐
│                   SparkEditor                           │
│  ┌────────────┐   ┌─────────┐   ┌───────────────────┐  │
│  │TextureSample│──│ Multiply │──│ SurfaceOutput     │  │
│  └────────────┘   └─────────┘   │  .albedo          │  │
│  ┌────────────┐   ┌─────────┐   │  .normal          │  │
│  │ WorldNormal│──│ Normalize│──│  .roughness        │  │
│  └────────────┘   └─────────┘   │  .metalness        │  │
│                                  └───────────────────┘  │
├─────────────────────────────────────────────────────────┤
│              ShaderGraphCompiler::Compile()              │
│  1. Find output node                                    │
│  2. Topological sort (backward from output)             │
│  3. Emit HLSL variable declarations                     │
│  4. Wire outputs to PBR struct                          │
├─────────────────────────────────────────────────────────┤
│              Generated HLSL                              │
│  vertex shader + pixel shader                           │
└─────────────────────────────────────────────────────────┘
```

---

## Node Types

The `ShaderNodeType` enum defines 35+ node types across four categories.

### Input Nodes

Provide data from the rendering context.

| Node | ID | Output | Description |
|------|----|--------|-------------|
| `TextureSample` | 0 | `float4` | Sample a texture at UV coordinates |
| `ConstantFloat` | 1 | `float` | Scalar constant value |
| `ConstantVec2` | 2 | `float2` | 2D vector constant |
| `ConstantVec3` | 3 | `float3` | 3D vector constant |
| `ConstantVec4` | 4 | `float4` | 4D vector constant |
| `ConstantColor` | 5 | `float4` | Color picker constant (RGBA) |
| `Time` | 6 | `float` | Elapsed time for animation |
| `UVCoords` | 7 | `float2` | Mesh UV coordinates |
| `WorldPosition` | 8 | `float3` | Fragment world position |
| `WorldNormal` | 9 | `float3` | Interpolated world normal |
| `CameraVector` | 10 | `float3` | Camera-to-fragment direction |

### Math Nodes

Perform mathematical operations on inputs.

| Node | ID | Inputs | Output | HLSL |
|------|----|--------|--------|------|
| `Add` | 50 | A, B | A + B | `a + b` |
| `Subtract` | 51 | A, B | A - B | `a - b` |
| `Multiply` | 52 | A, B | A * B | `a * b` |
| `Divide` | 53 | A, B | A / B | `a / b` |
| `DotProduct` | 54 | A, B | `float` | `dot(a, b)` |
| `CrossProduct` | 55 | A, B | `float3` | `cross(a, b)` |
| `Normalize` | 56 | V | unit V | `normalize(v)` |
| `Length` | 57 | V | `float` | `length(v)` |
| `Distance` | 58 | A, B | `float` | `distance(a, b)` |
| `Power` | 59 | Base, Exp | Base^Exp | `pow(base, exp)` |
| `Sqrt` | 60 | V | sqrt(V) | `sqrt(v)` |
| `Sin` | 61 | V | sin(V) | `sin(v)` |
| `Cos` | 62 | V | cos(V) | `cos(v)` |
| `Tan` | 63 | V | tan(V) | `tan(v)` |
| `Lerp` | 64 | A, B, T | lerp | `lerp(a, b, t)` |
| `Clamp` | 65 | V, Min, Max | clamped | `clamp(v, min, max)` |
| `Saturate` | 66 | V | [0,1] | `saturate(v)` |

### Utility Nodes

Higher-level operations.

| Node | ID | Description |
|------|----|-------------|
| `Fresnel` | 100 | Fresnel term from view angle and normal |
| `Noise` | 101 | Procedural noise generation |
| `SplitVector` | 105 | Split `float3/4` into individual components |
| `CombineVector` | 106 | Combine scalars into a vector |

### Output Nodes

Terminal nodes that define the material's surface properties.

| Node | ID | Inputs |
|------|----|--------|
| `SurfaceOutput` | 200 | Albedo, Normal, Roughness, Metalness, AO, Emissive (PBR) |
| `UnlitOutput` | 201 | Color, Alpha (unlit / UI materials) |

---

## Graph Structure

### ShaderNode

```cpp
struct ShaderNode
{
    uint32_t id;
    ShaderNodeType type;
    std::vector<ShaderNodeInput> inputs;
    std::unordered_map<std::string, std::string> properties;
};
```

Each node has a unique ID, a type, input sockets with default values, and optional string properties (e.g., texture path for `TextureSample`).

### ShaderConnection

```cpp
struct ShaderConnection
{
    uint32_t fromNodeID;
    uint32_t fromSocketIndex;
    uint32_t toNodeID;
    uint32_t toSocketIndex;
};
```

A connection wires one node's output socket to another node's input socket.

### ShaderGraphInput

```cpp
struct ShaderGraphInput
{
    std::vector<ShaderNode> nodes;
    std::vector<ShaderConnection> connections;
    uint32_t surfaceOutputNodeID;
    uint32_t unlitOutputNodeID;
    std::string materialName;
};
```

The complete graph description passed to the compiler. Either `surfaceOutputNodeID` or `unlitOutputNodeID` should be set (not both).

### ShaderGraphOutput

```cpp
struct ShaderGraphOutput
{
    std::string vertexShader;
    std::string pixelShader;
    std::vector<std::string> errors;
    int textureSlotCount;
    bool success;
};
```

The compilation result containing generated HLSL code, any error messages, and the number of texture slots used.

---

## Compilation Algorithm

`ShaderGraphCompiler::Compile()` follows a four-step process:

1. **Find the output node** — Locate the `SurfaceOutput` or `UnlitOutput` node in the graph
2. **Topological sort** — Walk connections backward from the output node to all reachable inputs, producing a dependency-ordered list of nodes
3. **Emit HLSL** — For each node in topological order, generate a local HLSL variable declaration. Connected inputs resolve to the upstream node's variable; unconnected inputs use default values
4. **Wire output struct** — Map the output node's inputs to the PBR or unlit output structure fields

Each node generates a uniquely named variable (`_node{id}_out{socket}`) to avoid collisions.

---

## Usage Example

```cpp
using namespace Spark::Graphics;

ShaderGraphInput graph;
graph.materialName = "BrickWall";

// Add a texture sample node
ShaderNode texNode;
texNode.id = 1;
texNode.type = ShaderNodeType::TextureSample;
texNode.properties["texture"] = "textures/brick_diffuse.dds";
graph.nodes.push_back(texNode);

// Add a surface output node
ShaderNode outputNode;
outputNode.id = 2;
outputNode.type = ShaderNodeType::SurfaceOutput;
outputNode.inputs.resize(6); // albedo, normal, roughness, metalness, ao, emissive
graph.nodes.push_back(outputNode);
graph.surfaceOutputNodeID = 2;

// Connect texture sample → albedo input
ShaderConnection conn;
conn.fromNodeID = 1;
conn.fromSocketIndex = 0;
conn.toNodeID = 2;
conn.toSocketIndex = 0; // albedo
graph.connections.push_back(conn);

// Compile
ShaderGraphOutput result = ShaderGraphCompiler::Compile(graph);
if (result.success)
{
    // result.vertexShader and result.pixelShader contain valid HLSL
    LOG_INFO("Generated shaders for {}", graph.materialName);
    LOG_INFO("Texture slots used: {}", result.textureSlotCount);
}
else
{
    for (const auto& err : result.errors)
    {
        LOG_ERROR("Shader graph error: {}", err);
    }
}
```

---

## Integration

- **SparkEditor**: The material editor panel creates `ShaderGraphInput` from the visual canvas and calls `Compile()`. See [SparkEditor](SparkEditor)
- **SparkShaderCompiler**: The standalone tool can also compile shader graphs from serialized JSON. See [Shader Pipeline](Shader-Pipeline)
- **Material System**: Generated HLSL is fed to the shader compilation pipeline and cached. See [Rendering and Graphics](Rendering-and-Graphics)
- **Thread safety**: Not thread-safe — compile one graph at a time

---

## See Also

- [Visual Scripting](Visual-Scripting) — General visual scripting system (references shader graph as prior art)
- [Shader Pipeline](Shader-Pipeline) — HLSL compilation, cross-compilation, and hot-reload
- [Rendering and Graphics](Rendering-and-Graphics) — Material system and PBR pipeline
- [Asset Pipeline](Asset-Pipeline) — Texture and material asset management
