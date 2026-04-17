# How SparkEngine Works (Beginner to Advanced)

This page explains SparkEngine in two layers:

1. **High level**: plain-English explanation for absolute beginners.
2. **Low level**: concrete technical flow (systems, frame order, ownership, threading).

If you only read one section, read **"The 60-Second Mental Model"** and **"One Frame, Step-by-Step"**.

---

## The 60-Second Mental Model

Think of SparkEngine like a restaurant kitchen:

- **EngineContext** is the head chef who knows where every station is.
- **Subsystems** (graphics, physics, audio, input, networking, scripting) are stations.
- **Game modules** are the menu for your game (FPS, RPG, MMO, etc.).
- **Main loop** is the repeating rhythm: _take orders -> cook -> plate -> repeat every frame_.

Every frame, SparkEngine:

1. Reads player/network input.
2. Updates simulation (physics, AI, gameplay logic).
3. Updates visuals/audio from latest simulation state.
4. Renders the frame.
5. Repeats.

---

## High-Level Architecture (No Jargon Version)

### 1) Boot
When the executable starts, it creates core systems and wires them into `EngineContext`.

### 2) Load game logic
The engine loads one or more game modules (`.dll`/`.so`) that contain game-specific rules.

### 3) Run loop
The engine runs a continuous loop until quit.

### 4) Shutdown
Modules unload first, then engine systems shut down in reverse dependency order.

That is the whole lifecycle.

---

## One Frame, Step-by-Step

This is the practical per-frame sequence used by SparkEngine's ECS/system orchestration:

1. **Input phase**
   - Keyboard/mouse/gamepad state gathered.
   - Network packets consumed.
2. **Simulation phase**
   - Physics integration and collision resolution.
   - Animation updates from movement/state.
   - AI decision updates from world state.
   - Audio/gameplay lifecycle updates.
3. **Render phase**
   - Renderer consumes final world/component state.
   - Post-processing chain runs.
   - UI composed.
4. **Present / end-of-frame work**
   - Frame metrics/profiling.
   - Deferred work queues.

### Canonical ECS order
SparkEngine's documented ECS execution order is:

**Physics -> Animation -> AI -> Audio -> Lifecycle -> Render**

This order avoids common dependency bugs (for example, animation reading stale physics data).

---

## Low-Level Breakdown (Technical)

## Core ownership model

- Engine systems are owned centrally (RAII; mostly `std::unique_ptr`).
- `EngineContext` exposes **non-owning pointers** for access.
- Named getters (`GetGraphics()`, `GetPhysics()`, etc.) are convenience APIs.
- Generic registry (`RegisterSystem<T>()` / `GetSystem<T>()`) supports extensibility.

**Why this matters:**
- Keeps ownership clear (fewer leaks/double frees).
- Allows game modules to access subsystems without singletons everywhere.

## Dependency-aware startup

SparkEngine supports dependency-aware subsystem registration.

- Each subsystem can declare dependencies (`DependsOn<...>`).
- `InitializeAll()` performs ordered startup (topological dependency order).
- `ShutdownAll()` runs reverse order.

**Why this matters:**
- Prevents "system used before initialized" crashes.
- Makes large engine startup predictable and testable.

## Module ABI boundary

Game code lives in dynamic modules implementing `IModule`.

Typical lifecycle:

1. `CreateModule()`
2. `GetModuleInfo()`
3. `OnLoad(IEngineContext*)`
4. Repeated `OnUpdate(deltaTime)` and optional `OnRender()`
5. `OnUnload()`
6. `DestroyModule()`

**Why this matters:**
- Engine and game logic are decoupled.
- You can ship multiple gameplay modules with one engine core.

## Rendering architecture

SparkEngine uses an RHI abstraction with multiple backends (D3D11 primary, plus experimental D3D12/Vulkan/Metal/OpenGL and NullRHI fallback).

Render path at a high level:

1. Collect visible scene state.
2. Build/execute render passes.
3. Run post-processing stack.
4. Composite UI.
5. Present.

If no GPU backend is available, NullRHI/headless fallback keeps engine logic running.

## Threading model (practical summary)

- **Main/game thread**: orchestration, gameplay state ownership.
- **Physics jobs**: multithread-capable via physics job dispatch.
- **Network message queues**: mutex-protected handoff.
- **Render submission**: generally main-thread coordinated, backend-dependent internals.

Rule of thumb for contributors:

- If a system owns gameplay state, treat it as game-thread authoritative unless explicitly designed otherwise.
- Use queues/events for cross-thread boundaries, not shared mutable state without synchronization.

---

## "Explain it like I'm brand new" walkthrough

Imagine pressing **W** to move forward:

1. Input system marks W as pressed.
2. Player movement system requests velocity change.
3. Physics system moves the character capsule and resolves collisions.
4. Animation system chooses run/walk blend based on resulting speed.
5. Camera system updates final camera transform.
6. Audio system plays footstep events if movement state changed.
7. Renderer draws world from the new camera/player position.
8. You see movement on screen this frame.

Same idea applies to shooting, interacting, opening doors, etc.

---

## Mental checklist for understanding any SparkEngine subsystem

When reading a new subsystem, answer these 6 questions:

1. **Who owns it?** (`unique_ptr` owner or external owner?)
2. **Who initializes it?** (EngineContext/manual lifecycle?)
3. **When is it updated?** (which phase/order?)
4. **What data does it read/write?** (components, events, resources)
5. **Thread affinity?** (game thread only, worker-safe, or mixed?)
6. **How does it fail?** (assert/log/expected return path?)

If these are clear, the subsystem is usually easy to reason about.

---

## Where to go next

If this page made sense, continue in this order:

1. [Architecture Overview](Architecture-Overview.md)
2. [Engine Architecture Flowchart](Engine-Architecture-Flowchart.md)
3. [Threading Model](../advanced/Threading-Model.md)
4. [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md)
5. [Entity Component System](../subsystems/Entity-Component-System.md)

