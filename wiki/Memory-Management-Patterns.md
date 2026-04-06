# Memory Management Patterns

This page documents the ownership rules, allocation strategies, and debugging tools used throughout SparkEngine.

**Source:** `SparkEngine/Source/Utils/FrameAllocator.h`, `MemoryDebugger.h`, `SparkEngine/Source/Graphics/RenderTargetPool.h`, `GameModules/SparkGame/Source/Projectiles/ProjectilePool.h`

---

## Ownership Rules

SparkEngine follows strict ownership conventions to prevent leaks and dangling pointers:

| Pattern | When to use | Example |
|---------|-------------|---------|
| `std::unique_ptr<T>` | Single owner, transferable | `std::unique_ptr<BehaviorTree> m_behaviorTree` |
| `ComPtr<T>` | D3D11/DXGI COM objects | `ComPtr<ID3D11Buffer> m_vertexBuffer` |
| Raw pointer `T*` | Non-owning reference | `Player* m_player` (observer, not owner) |
| `std::shared_ptr<T>` | Shared ownership (rare) | `std::shared_ptr<Asset>` in asset cache |

### Key Rules

1. **No naked `new`/`delete`.** All heap allocations use smart pointers or pool allocators.
2. **`unique_ptr` is the default.** Use `std::make_unique<T>()` for all single-owner heap objects.
3. **Raw pointers are non-owning.** A raw `T*` never implies the holder should delete the object.
4. **COM objects use `ComPtr`.** All D3D11 resources (`ID3D11Buffer`, `ID3D11Texture2D`, etc.) are wrapped in `Microsoft::WRL::ComPtr` for automatic `Release()`.
5. **RAII everywhere.** Resources are released in destructors — no manual cleanup calls in normal flow.

---

## Frame Allocator

**Source:** `SparkEngine/Source/Utils/FrameAllocator.h`

A linear (bump) allocator for per-frame temporary allocations. Memory is allocated in O(1) by advancing a pointer and freed in O(1) by resetting the offset to zero.

### Interface

```cpp
class FrameAllocator {
public:
    explicit FrameAllocator(size_t capacityBytes = 1024 * 1024);  // Default 1 MB

    void* Allocate(size_t size, size_t alignment = 16);
    template<typename T> T* Alloc(size_t count = 1);
    template<typename T, typename...Args> T* New(Args&&... args);

    void Reset();           // O(1) — resets offset, no destructors called
    size_t Used() const;
    size_t Remaining() const;
    size_t PeakUsed() const;
};
```

### Usage Pattern

```cpp
// At engine startup
FrameAllocator frameAlloc(2 * 1024 * 1024);  // 2 MB

// Each frame
frameAlloc.Reset();

// Allocate temporary data (no free needed)
auto* drawCmds = frameAlloc.Alloc<DrawCommand>(256);
auto* lights = frameAlloc.Alloc<LightData>(maxLights);
```

### Constraints

- Only for **trivially destructible** types — `Reset()` does not call destructors
- Allocations are **not individually freeable** — the entire buffer resets at once
- Thread-unsafe — use one allocator per thread if needed

---

## Render Target Pool

**Source:** `SparkEngine/Source/Graphics/RenderTargetPool.h`

Pools GPU render targets by descriptor (format, dimensions, sample count) to avoid creating and destroying textures every frame.

### Interface

```cpp
struct RenderTargetDesc {
    uint32_t width, height;
    DXGI_FORMAT format;
    uint32_t sampleCount, mipLevels;
    bool isDepthStencil;
};

class RenderTargetPool {
public:
    PooledRTHandle Acquire(const RenderTargetDesc& desc);
    void Release(PooledRTHandle handle);
    void Tick();  // Reclaim targets idle for N frames

    ID3D11RenderTargetView* GetRTV(PooledRTHandle handle);
    ID3D11ShaderResourceView* GetSRV(PooledRTHandle handle);
    ID3D11Texture2D* GetTexture(PooledRTHandle handle);

    RenderTargetPoolMetrics GetMetrics() const;
};
```

### Usage Pattern

```cpp
// Acquire a target for this frame's bloom pass
auto bloomRT = pool.Acquire({width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, false});

// Render bloom pass using GetRTV(bloomRT)...

// Release back to pool (available for reuse next frame)
pool.Release(bloomRT);

// Each frame, reclaim stale targets
pool.Tick();
```

### Design Notes

- Targets idle for 60+ frames are automatically destroyed
- Depth/stencil targets use typeless formats for SRV compatibility
- All internal D3D11 resources use `ComPtr` for RAII

---

## Object Pool (ProjectilePool)

**Source:** `GameModules/SparkGame/Source/Projectiles/ProjectilePool.h`

Pre-allocates a fixed number of game objects and recycles them to avoid runtime allocation:

```cpp
class ProjectilePool {
public:
    ProjectilePool(size_t poolSize);

    Projectile* GetProjectile();        // O(1) from free list
    void ReturnProjectile(Projectile* p); // O(1) back to free list

    size_t GetActiveCount() const;
    size_t GetAvailableCount() const;
};
```

### Storage

```cpp
std::vector<std::unique_ptr<Projectile>> m_projectiles;  // Owns all objects
std::queue<Projectile*> m_availableProjectiles;           // Free list (non-owning)
```

### Pattern

This pattern applies to any frequently spawned/despawned object: particles, decals, audio sources. The pool owns all objects via `unique_ptr`; the free queue holds non-owning pointers.

---

## Memory Debugger

**Source:** `SparkEngine/Source/Utils/MemoryDebugger.h`

Debug-build allocation tracker that records every allocation with source location and category, then reports leaks at shutdown.

### Recording Allocations

```cpp
// Manual tracking
MemoryDebugger::GetInstance().RecordAlloc(ptr, size, "Physics", __FILE__, __LINE__, __func__);
MemoryDebugger::GetInstance().RecordFree(ptr);

// Convenience macros
SPARK_TRACK_ALLOC(ptr, size, "Rendering");
SPARK_TRACK_FREE(ptr);
```

### Leak Detection

```cpp
auto leaks = MemoryDebugger::GetInstance().GetLeaks();
for (const auto& leak : leaks) {
    // leak.address, leak.size, leak.category, leak.location
}

SPARK_PRINT_LEAK_REPORT();  // Outputs to console
```

### Category Statistics

```cpp
auto stats = MemoryDebugger::GetInstance().GetCategoryStats();
// stats[i].name, stats[i].currentBytes, stats[i].peakBytes,
// stats[i].totalAllocations, stats[i].totalDeallocations

auto hotspots = MemoryDebugger::GetInstance().GetHotSpots(10);
// Top 10 allocation sites by frequency
```

---

## D3D11 Resource Management

All GPU resources follow this pattern:

```cpp
class SomeRenderer {
private:
    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11Buffer> m_indexBuffer;
    ComPtr<ID3D11ShaderResourceView> m_textureSRV;
    ComPtr<ID3D11RenderTargetView> m_renderTarget;
};
// ComPtr calls Release() automatically in destructor
```

### Resource Creation

```cpp
ComPtr<ID3D11Buffer> buffer;
D3D11_BUFFER_DESC desc = {};
desc.ByteWidth = sizeof(Vertex) * vertexCount;
desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
desc.Usage = D3D11_USAGE_DEFAULT;

D3D11_SUBRESOURCE_DATA initData = {};
initData.pSysMem = vertices.data();

HRESULT hr = device->CreateBuffer(&desc, &initData, buffer.GetAddressOf());
```

---

## Guidelines

1. **Prefer stack allocation** for small, short-lived objects
2. **Use FrameAllocator** for per-frame temporary data (draw lists, scratch buffers)
3. **Use object pools** for frequently spawned/despawned objects (projectiles, particles)
4. **Use RenderTargetPool** for GPU render targets that vary by frame
5. **Use `unique_ptr`** for everything else on the heap
6. **Use `shared_ptr`** only when true shared ownership is required (asset cache)
7. **Enable MemoryDebugger** in debug builds to catch leaks early
8. **Never use raw `new`/`delete`** — always wrap in a smart pointer or pool

See [Profiler and Debugging](Profiler-and-Debugging) for runtime memory profiling tools.

See [Memory Safety](Memory-Safety) for type-safe utilities (NonNull, SafeCast, Contracts) and compiler hardening.
