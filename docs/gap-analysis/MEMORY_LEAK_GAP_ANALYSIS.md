# SparkEngine Memory Leak — Gap Analysis

> **Scope**: Engine-wide memory management — ownership, RAII, container growth, resource lifecycle
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of allocation patterns, destructor correctness, container lifecycle, and resource ownership across all subsystems.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

SparkEngine's CLAUDE.md coding standards mandate `std::unique_ptr` for owning pointers, raw pointers for non-owning references, no naked `new`/`delete`, and RAII for all resource management. The codebase largely follows these rules in the ECS, audio, and editor subsystems. However, the Bullet Physics integration uses extensive naked `new`, the event system has callback lifecycle issues, and several containers grow without bounds. These issues create memory leak risks ranging from slow accumulation to immediate corruption.

---

## Critical Gaps

### GAP-ML01 — Bullet Physics Objects Allocated with Naked `new`

**Files**:
- `Physics/PhysicsSystem.cpp` (lines 640–648)

**Impact**: Five core Bullet world objects are allocated with raw `new` and stored as raw pointers:

```cpp
m_collisionConfig = new btDefaultCollisionConfiguration();
m_dispatcher = new btCollisionDispatcher(m_collisionConfig);
m_broadphase = new btDbvtBroadphase();
m_solver = new btSequentialImpulseConstraintSolver();
m_dynamicsWorld = new btDiscreteDynamicsWorld(...);
```

While `Shutdown()` (lines 694–708) deletes them in reverse order, any exception or early return between `Initialize()` and `Shutdown()` leaks all five objects. If `Shutdown()` is never called (crash, forced exit), all Bullet memory leaks. Additionally, a `btGhostPairCallback` at line 648 is allocated with `new` and passed to the broadphase with no stored pointer — it is never explicitly deleted.

**Evidence**: Lines 640–648 use naked `new`. Lines 694–708 use naked `delete`. No `std::unique_ptr` wrapping. No RAII guard.

**What is needed**: Wrap each Bullet object in `std::unique_ptr` with a custom deleter that handles the reverse-order destruction dependency. At minimum, use a destructor guard that calls `Shutdown()` if not already called.

---

### GAP-ML02 — PhysicsBody Destructor Double-Deletes Collision Shapes

**Files**:
- `Physics/PhysicsSystem.cpp` (lines 190–199, 688–692)

**Impact**: `PhysicsBody::~PhysicsBody()` deletes the collision shape retrieved from the Bullet body:

```cpp
PhysicsBody::~PhysicsBody()
{
    if (m_bulletBody)
    {
        delete m_bulletBody->getMotionState();
        delete m_bulletBody->getCollisionShape();  // Shape from m_shapeCache
        delete m_bulletBody;
    }
}
```

But collision shapes are also cached and deleted in `PhysicsSystem::Shutdown()`:

```cpp
for (auto& [hash, shape] : m_shapeCache)
    delete shape;  // Same shape already deleted by ~PhysicsBody
```

If bodies are destroyed before `Shutdown()` (normal case), the shape is deleted by `~PhysicsBody`. When `Shutdown()` then iterates `m_shapeCache`, it deletes the same pointer again — **double-free/undefined behavior**. If bodies are destroyed *after* `Shutdown()` clears the cache, the body's destructor deletes a shape that was already freed.

**What is needed**: Choose a single owner for collision shapes. Either: (a) `m_shapeCache` owns shapes and `PhysicsBody` holds a non-owning pointer (remove the `delete` from `~PhysicsBody`), or (b) each body owns its shape exclusively (remove from cache on body destruction). Option (a) is simpler and matches the caching pattern.

---

### GAP-ML03 — Collision Shape Creation Returns Raw `new` Pointers

**Files**:
- `Physics/PhysicsSystem.cpp` (lines 82–153)

**Impact**: Shape factory methods use naked `new` for all Bullet collision shapes:

```cpp
shape = new btCylinderShape(...);
shape = new btConeShape(...);
return new btBoxShape(...);
return new btSphereShape(...);
btTriangleMesh* triMesh = new btTriangleMesh();
btBvhTriangleMeshShape* meshShape = new btBvhTriangleMeshShape(triMesh, true);
btConvexHullShape* convexShape = new btConvexHullShape();
```

These are stored in `m_shapeCache` (raw pointer map) with no RAII. If shape creation fails partway through (e.g., `btBvhTriangleMeshShape` construction throws after `triMesh` is allocated), intermediate objects leak.

**What is needed**: Use `std::unique_ptr<btCollisionShape>` in `m_shapeCache`. Use `std::make_unique` or `std::unique_ptr` locals during creation to guarantee cleanup on exception.

---

## Major Gaps

### GAP-ML04 — Event Subscription Callbacks Prevent Object Destruction

**Files**:
- `Engine/Events/EventSystem.h` (lines 84–91)

**Impact**: `EventBus::Subscribe<T>()` stores callbacks as `std::function<void(const void*)>` that capture arbitrary state via lambda closures. If a subscriber captures `this` (common pattern: `bus.Subscribe<Event>([this](const Event& e) { ... })`), the `std::function` holds a copy of the pointer. If the subscribing object is destroyed without calling `Unsubscribe()`, the callback becomes a dangling pointer — invoking it causes use-after-free.

There is no mechanism to automatically unsubscribe when an object is destroyed. No RAII subscription handle is returned. No weak reference pattern is used.

**Evidence**: `Subscribe()` returns a `SubscriptionID` (uint64_t). If the subscriber loses this ID or doesn't store it, there is no way to unsubscribe. The `EventBus` has no way to detect that a subscriber's captured `this` pointer is invalid.

**What is needed**: Return an RAII subscription handle (similar to `std::unique_lock`) that automatically calls `Unsubscribe()` in its destructor. Alternatively, use `std::weak_ptr`-based subscriptions that automatically expire when the subscriber is destroyed.

---

### GAP-ML05 — Unbounded Thread Vector Growth in InputManager

**Files**:
- `Input/InputManager.h` (line 83)
- `Input/InputManager.cpp` (lines 456–463)

**Impact**: `Console_SimulateKeyPress()` with a duration spawns a `std::thread` and appends it to `m_pendingTimedThreads`. Cleanup removes non-joinable threads, but completed threads remain joinable until explicitly joined. The vector grows with each timed key simulation and is only fully cleaned up in the destructor.

**Evidence**:
```cpp
m_pendingTimedThreads.erase(std::remove_if(...,
    [](std::thread& t) { return !t.joinable(); }), ...);
m_pendingTimedThreads.emplace_back([this, virtualKey, keyName, duration]() { ... });
```

Completed threads are still joinable — they are only non-joinable after being joined or moved. The cleanup predicate (`!t.joinable()`) never matches completed threads, so the vector only grows.

**What is needed**: Join completed threads before erasing. Use `std::jthread` (C++20) which auto-joins on destruction, or track thread completion with an atomic flag and join finished threads during cleanup.

---

### GAP-ML06 — RHI Resource Factory Returns Raw `new` Pointers

**Files**:
- `Graphics/RHI/D3D11/D3D11Device.cpp` (lines 614, 670, 786, 808, 904, 982)

**Impact**: All D3D11 RHI resource factory methods return raw pointers from `new`:

```cpp
return new D3D11Buffer(desc, std::move(buffer));         // line 614
return new D3D11Texture(desc, texture, std::move(srv), std::move(rtv), std::move(dsv)); // line 670
return new D3D11Shader(desc, std::move(shaderObj), std::move(bytecodeBlob));  // line 786
return new D3D11Sampler(desc, std::move(sampler));       // line 808
return new D3D11PipelineState(desc, std::move(inputLayout), ...);  // line 904
```

Callers must manually call corresponding `Destroy*` methods or the GPU resources leak indefinitely. If a caller forgets, or if an exception occurs between creation and destruction, the resource leaks both CPU memory (the wrapper object) and GPU memory (the underlying D3D11 resource).

**What is needed**: Return `std::unique_ptr<IRHIBuffer>`, `std::unique_ptr<IRHITexture>`, etc. from factory methods. This enforces RAII ownership and prevents leaks from forgotten cleanup.

---

## Moderate Gaps

### GAP-ML07 — Audio Source Pool Uses Raw Pointers Alongside unique_ptr

**Files**:
- `Audio/AudioEngine.cpp` (lines 75–82)

**Impact**: Audio sources are owned by `m_audioSources` (vector of `unique_ptr`) but tracked in `m_availableSources` as raw pointers:

```cpp
auto source = std::make_unique<AudioSource>();
m_availableSources.push_back(source.get());
m_audioSources.push_back(std::move(source));
```

If `m_audioSources` is resized (reallocation), all raw pointers in `m_availableSources` become dangling. While the pool is initialized once and doesn't grow, any future code that adds sources after initialization would trigger reallocation and invalidation.

**What is needed**: Use stable containers (`std::deque` or `std::list`) for `m_audioSources`, or pre-allocate with `reserve()` and document the invariant that the vector must not grow after initialization.

---

### GAP-ML08 — Singleton Systems Have No Cleanup Ordering

**Files**:
- `Engine/SaveSystem/SaveSystem.h`, `Engine/Coroutine/CoroutineScheduler.h`, `Engine/Cinematic/Sequencer.h`

**Impact**: Multiple engine singletons (`SaveSystem`, `CoroutineScheduler`, `SequencerManager`) use Meyer's singleton pattern (local static). Their destruction order during program exit is the reverse of first-access order, which is unpredictable. If singleton A's destructor accesses singleton B, and B was destroyed first, this is use-after-free.

**Evidence**: All three singletons use the pattern:
```cpp
static SaveSystem& GetInstance() { static SaveSystem instance; return instance; }
```

No explicit `Shutdown()` is called before static destruction. No dependency ordering is enforced.

**What is needed**: Add explicit `Initialize()`/`Shutdown()` methods with a defined call order in the engine's main loop shutdown sequence. Store singletons as `EngineContext`-owned `unique_ptr`s with deterministic destruction order.

---

### GAP-ML09 — No Leak Detection Tooling Integration

**Files**: Engine-wide

**Impact**: The engine has no integration with memory leak detection tools. No custom allocator tracks outstanding allocations. No debug-mode memory fill pattern detects use-after-free. The `Profiler` tracks timing but not memory usage.

**What is needed**: In debug builds, integrate a leak detection system: override `operator new`/`delete` to track allocations with source file/line, or enable MSVC's CRT leak detection (`_CrtSetDbgFlag`). Add a `MemoryTracker` that logs outstanding allocations on shutdown.

---

## Minor Gaps

### GAP-ML10 — Recent Input Events Vector Never Shrinks

**Files**:
- `Input/InputManager.h` (line 79, `m_recentInputEvents`)
- `Input/InputManager.cpp` (lines 744–748)

**Impact**: `m_recentInputEvents` is capped at 100 entries via front-erase, but the vector's capacity never shrinks. After 100 entries, the vector holds 100 elements but may have capacity for thousands (from earlier growth). The unused capacity is wasted memory.

**What is needed**: Use a fixed-size ring buffer (the engine's `Utils/RingBuffer`) instead of a dynamic vector. This bounds both size and capacity.

---

### GAP-ML11 — PhysicsBody MotionState Deleted Without Ownership Clarity

**Files**:
- `Physics/PhysicsSystem.cpp` (lines 194, 872)

**Impact**: `btDefaultMotionState` is allocated with `new` at line 872 and stored inside the `btRigidBody`. The `PhysicsBody` destructor deletes it at line 194 via `delete m_bulletBody->getMotionState()`. This works correctly but the ownership is implicit — there is no documentation that `PhysicsBody` owns the motion state. If Bullet's internal code also deletes the motion state (it doesn't by default, but a custom body subclass might), this becomes a double-free.

**What is needed**: Store the motion state in a `std::unique_ptr<btDefaultMotionState>` member of `PhysicsBody` and pass the raw pointer to `btRigidBody`. Ownership is then explicit and documented by the type system.

---
