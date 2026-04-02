# Error Handling Patterns

SparkEngine uses multiple error handling strategies across its subsystems, chosen based on the boundary type and failure severity. This page documents the conventions, when to use each pattern, and how errors propagate through the engine.

**Relevant headers:** `SparkEngine/Source/Utils/Assert.h`, `SparkEngine/Source/Utils/Logger.h`, `SparkEngine/Source/Core/Platform.h`
**Namespace:** `Spark`

---

## Table of Contents

- [Philosophy](#philosophy)
- [Error Handling Strategies](#error-handling-strategies)
  - [bool Return Values](#bool-return-values)
  - [HRESULT (D3D11/COM)](#hresult-d3d11com)
  - [std::expected (C++23)](#stdexpected-c23)
  - [Assertions](#assertions)
  - [Error Callbacks](#error-callbacks)
- [When to Use What](#when-to-use-what)
- [Error Logging](#error-logging)
- [Error Propagation Patterns](#error-propagation-patterns)
- [Subsystem Conventions](#subsystem-conventions)
- [See Also](#see-also)

---

## Philosophy

SparkEngine follows these principles for error handling:

1. **Fail loudly in development, gracefully in production** — Assertions catch programmer errors during development; runtime error paths handle user/environment failures
2. **Validate at system boundaries** — Validate user input, file I/O, network data, and API calls. Trust internal engine code
3. **Return errors, don't throw exceptions** — SparkEngine does not use C++ exceptions. All error paths use return values
4. **Log before returning** — Every error return should be accompanied by a `LOG_ERROR` or `LOG_WARN` so failures are always visible

---

## Error Handling Strategies

### bool Return Values

The most common pattern across the engine. `Initialize()`, `LoadAsset()`, and similar functions return `true` on success:

```cpp
bool AudioEngine::Initialize()
{
    if (!CreateXAudioDevice())
    {
        LOG_ERROR("Failed to create XAudio2 device");
        return false;
    }

    m_initialized = true;
    return true;
}
```

**Use for**: Engine subsystem initialization, resource loading, operations with a clear pass/fail outcome.

### HRESULT (D3D11/COM)

Windows graphics and COM APIs return `HRESULT`. SparkEngine propagates these directly in graphics code:

```cpp
HRESULT GraphicsEngine::CreateDevice()
{
    HRESULT hr = D3D11CreateDevice(/* ... */);
    if (FAILED(hr))
    {
        LOG_ERROR("D3D11CreateDevice failed: 0x{:08X}", static_cast<uint32_t>(hr));
        return hr;
    }
    return S_OK;
}
```

**Use for**: D3D11/D3D12 resource creation, shader compilation, any COM interop. Always log the hex HRESULT value for debugging.

### std::expected (C++23)

For operations that can fail with a meaningful error value, `std::expected` provides a type-safe result-or-error:

```cpp
std::expected<Mesh, std::string> LoadMesh(const std::string& path)
{
    auto data = ReadFile(path);
    if (!data)
    {
        return std::unexpected("File not found: " + path);
    }

    Mesh mesh = ParseMeshData(*data);
    if (mesh.vertices.empty())
    {
        return std::unexpected("Empty mesh: " + path);
    }

    return mesh;
}

// Caller:
auto result = LoadMesh("models/hero.fbx");
if (result)
{
    UseModel(*result);
}
else
{
    LOG_ERROR("{}", result.error());
}
```

**Use for**: Operations where the caller needs to know *what* went wrong, not just that it failed. Preferred for new code over raw bool returns when error context matters.

### Assertions

Assertions catch programming errors — conditions that should never occur if the code is correct:

```cpp
#include "Utils/Assert.h"

void PhysicsSystem::AddBody(PhysicsBody* body)
{
    SPARK_ASSERT(body != nullptr);
    SPARK_ASSERT(m_initialized && "PhysicsSystem not initialized");

    m_bodies.push_back(body);
}
```

**Rules:**
- Assertions are **enabled in Debug**, **disabled in Release**
- Never put side effects inside an assertion expression
- Use assertions for programmer errors (null pointers, invalid states, violated preconditions)
- Do NOT use assertions for runtime failures (file not found, network error, user input)

### Error Callbacks

Some subsystems use callback-based error reporting, especially for async operations:

```cpp
// AngelScript compilation errors
scriptEngine.SetMessageCallback([](const std::string& msg, int line)
{
    LOG_ERROR("Script error at line {}: {}", line, msg);
});

// Network error handling
networkManager.SetErrorHandler([](NetworkError error, const std::string& detail)
{
    LOG_ERROR("Network error {}: {}", static_cast<int>(error), detail);
});
```

**Use for**: Async operations, third-party library integration, situations where errors are reported to a different context than the caller.

---

## When to Use What

| Situation | Pattern | Example |
|-----------|---------|---------|
| Subsystem `Initialize()` | `bool` return | `AudioEngine::Initialize()` |
| D3D11/COM API call | `HRESULT` | `CreateBuffer()`, `CompileShader()` |
| File/asset loading | `std::expected` or `bool` | `LoadMesh()`, `LoadTexture()` |
| Programmer error (impossible state) | `SPARK_ASSERT` | Null pointer, uninitialized system |
| Network message parsing | `bool` with `HasError()` | `NetBuffer::ReadString()` |
| Script compilation | Error callback | `SetMessageCallback()` |
| Configuration validation | `bool` + `LOG_WARN` | Invalid settings clamped to valid range |

---

## Error Logging

All errors should be logged through the `Logger` system:

```cpp
#include "Utils/Logger.h"

LOG_ERROR("Failed to load texture: {}", path);    // Errors (something broke)
LOG_WARN("Texture format unsupported, using fallback");  // Warnings (degraded but functional)
LOG_INFO("Loaded {} textures", count);             // Info (normal operation)
```

**Guidelines:**
- `LOG_ERROR`: Something failed and the operation cannot complete
- `LOG_WARN`: Something unexpected happened but the engine can continue (e.g., fallback used)
- `LOG_INFO`: Normal operational messages (use sparingly in hot paths)
- Include context in the message: file paths, counts, error codes
- Never log inside tight loops — guard with `if` or rate-limit

---

## Error Propagation Patterns

### Early Return

The predominant pattern. Check each operation and return immediately on failure:

```cpp
bool Scene::Load(const std::string& path)
{
    auto data = ReadFile(path);
    if (!data) return false;

    if (!ParseHeader(*data)) return false;
    if (!LoadEntities(*data)) return false;
    if (!LoadLighting(*data)) return false;

    return true;
}
```

### Accumulate and Report

For operations that should try to complete even with partial failures:

```cpp
ShaderGraphOutput ShaderGraphCompiler::Compile(const ShaderGraphInput& graph)
{
    ShaderGraphOutput output;

    if (graph.nodes.empty())
    {
        output.errors.push_back("Graph has no nodes");
    }

    // Continue checking...
    if (!FindOutputNode(graph))
    {
        output.errors.push_back("No output node found");
    }

    output.success = output.errors.empty();
    return output;
}
```

### NetBuffer Overflow Protection

Network buffers track an internal error flag:

```cpp
NetBuffer buf(receivedData);
uint32_t id = buf.ReadUInt32();
std::string name = buf.ReadString();

if (buf.HasError())
{
    LOG_WARN("Malformed packet: buffer overflow");
    return;
}
```

---

## Subsystem Conventions

| Subsystem | Primary Pattern | Notes |
|-----------|----------------|-------|
| Graphics (D3D11) | `HRESULT` | COM convention, always check `FAILED()` |
| Physics (Jolt) | `bool` + assertions | Jolt uses assertions internally |
| Audio (XAudio2) | `HRESULT` | COM-based API |
| Networking | `bool` + `HasError()` | NetBuffer overflow tracking |
| Scripting | Error callbacks | AngelScript message callback |
| ECS | Assertions | Invalid entity/component access |
| Asset Loading | `bool` or `std::expected` | File I/O at system boundary |
| Scene Management | `bool` early return | Chain of loading steps |

---

## See Also

- [Utilities](Utilities) — Logger and Assert implementations
- [Testing](Testing) — How errors are tested
- [Profiler and Debugging](Profiler-and-Debugging) — Runtime diagnostics
- [Contributing](Contributing) — Coding standards for new code
