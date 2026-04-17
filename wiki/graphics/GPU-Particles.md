# GPU Particles

SparkEngine provides a GPU-accelerated particle system that runs emission, simulation, sorting, and rendering entirely on the GPU via D3D11 compute shaders. Each emitter supports up to 1 million particles with bitonic sort for correct alpha blending.

**Source:** `SparkEngine/Source/Graphics/GPUParticleSystem.h`, `SparkEngine/Source/Graphics/GPUParticleTypes.h`
**Namespace:** Global (classes prefixed with `GPU`)
**Tests:** `Tests/TestGPUParticleSystem.cpp` (11 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Pipeline](#pipeline)
- [Emitter Configuration](#emitter-configuration)
- [GPU Data Layout](#gpu-data-layout)
  - [GPUParticleData](#gpuparticledata)
  - [GPUParticleEmitterCB](#gpuparticleemittercb)
  - [Buffer Architecture](#buffer-architecture)
- [API Reference](#api-reference)
  - [Initialization](#initialization)
  - [Emitter Management](#emitter-management)
  - [Simulation and Rendering](#simulation-and-rendering)
  - [Emitter Control](#emitter-control)
- [Usage Example](#usage-example)
- [Performance](#performance)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

The GPU particle system offloads all particle work to compute shaders, freeing the CPU for gameplay logic. The CPU only manages emitter configuration and issues dispatch calls.

```
┌─────────────────────────────────────────────────────────────┐
│                         CPU Side                            │
│  CreateEmitter() → configure desc → Update(dt) → Render()  │
├─────────────────────────────────────────────────────────────┤
│                         GPU Side                            │
│  ┌──────────────┐  ┌────────────────┐  ┌─────────────────┐ │
│  │ ParticleEmit │→│ParticleSimulate│→│ParticleBitonic  │ │
│  │ .hlsl        │  │ .hlsl          │  │ Sort.hlsl       │ │
│  │              │  │                │  │                 │ │
│  │ Pop dead     │  │ Integrate vel  │  │ Sort alive by   │ │
│  │ Init new     │  │ Kill expired   │  │ camera distance │ │
│  │ particles    │  │ Compact alive  │  │                 │ │
│  └──────────────┘  └────────────────┘  └─────────────────┘ │
│                           |                                 │
│                           v                                 │
│                  DrawInstancedIndirect                       │
│                  (alive list + particle pool SRVs)           │
└─────────────────────────────────────────────────────────────┘
```

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `kMaxGPUParticlesPerEmitter` | 1,048,576 (1M) | Hard maximum particles per emitter |
| `kDefaultGPUParticleCapacity` | 65,536 (64K) | Default capacity if not specified |
| `kParticleThreadGroupSize` | 256 | Compute shader thread group size |

---

## Pipeline

Each frame, `Update(deltaTime)` dispatches three compute shader passes per emitter:

1. **Emit** (`ParticleEmit.hlsl`): Pops indices from the dead list, initializes new particles with position, velocity, lifetime, color, and rotation based on emitter settings
2. **Simulate** (`ParticleSimulate.hlsl`): Integrates velocity with gravity and drag, ages particles, kills expired ones (push to dead list), and compacts the alive list
3. **Bitonic Sort** (`ParticleBitonicSort.hlsl`): Sorts the alive list by camera distance for correct back-to-front alpha blending
4. **Update indirect args**: Writes the alive count into the indirect draw argument buffer

`Render(view, projection)` then binds the alive list SRV and particle pool SRV and issues `DrawInstancedIndirect`.

---

## Emitter Configuration

The `ParticleEmitterDesc` (defined in `GPUParticleTypes.h`) configures emitter behavior:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `string` | `""` | Debug identifier |
| `emissionRate` | `float` | 100.0 | Particles emitted per second |
| `emitterShape` | `EmitterShape` | Point | Point, Sphere, Cone, Box |
| `shapeRadius` | `float` | 1.0 | Radius for Sphere/Cone shapes |
| `coneAngle` | `float` | 45.0 | Half-angle in degrees for Cone |
| `gravity` | `XMFLOAT3` | {0,-9.8,0} | Gravity acceleration |
| `drag` | `float` | 0.0 | Velocity damping per second |
| `gravityMultiplier` | `float` | 1.0 | Scale factor for gravity |
| `lifetimeRange` | x=min, y=max | 1.0-3.0 | Random lifetime range (seconds) |
| `sizeRange` | x=min, y=max | 0.1-0.5 | Random particle size range |
| `rotSpeedRange` | x=min, y=max | 0.0-0.0 | Random rotation speed (rad/s) |

---

## GPU Data Layout

### GPUParticleData

The per-particle structured buffer layout (16-byte aligned):

```cpp
struct alignas(16) GPUParticleData
{
    XMFLOAT3 position;
    float age;
    XMFLOAT3 velocity;
    float lifetime;
    XMFLOAT4 color;
    float size;
    float rotation;
    float rotationSpeed;
    uint32_t alive;     // 1 = alive, 0 = dead
};
```

### GPUParticleEmitterCB

Per-emitter constant buffer sent to compute shaders each frame:

```cpp
struct alignas(16) GPUParticleEmitterCB
{
    XMFLOAT3 emitterPosition;
    float deltaTime;
    XMFLOAT3 gravity;
    float drag;
    float emissionRate;
    float shapeRadius;
    float coneAngle;
    uint32_t maxParticles;
    XMFLOAT4 lifetimeRange;  // x=min, y=max, z=speedMin, w=speedMax
    XMFLOAT4 sizeRange;      // x=sizeMin, y=sizeMax, z=rotMin, w=rotMax
    XMFLOAT4 rotSpeedRange;  // x=min, y=max
    uint32_t emitCount;       // particles to emit this frame
    uint32_t emitterShape;
    float gravityMultiplier;
    float totalTime;
};
```

### Buffer Architecture

Each emitter owns seven GPU buffers:

| Buffer | Type | Description |
|--------|------|-------------|
| `particleBuffer` | RW Structured | Pool of all particle data |
| `deadListBuffer` | Append/Consume | Available particle indices |
| `aliveListBuffer` | RW Structured | This frame's alive particle indices |
| `aliveListSwapBuffer` | RW Structured | Previous frame's alive indices (double-buffered) |
| `sortKeysBuffer` | RW Structured | Camera distance keys for bitonic sort |
| `indirectArgsBuffer` | Indirect | `DrawInstancedIndirect` arguments |
| `emitterCBBuffer` | Constant | Per-emitter constants |

---

## API Reference

### Initialization

```cpp
auto& gpu = GPUParticleSystem::GetInstance();

// Windows (D3D11)
HRESULT hr = gpu.Initialize(device, context);

// Non-Windows (stub)
bool ok = gpu.Initialize();
```

### Emitter Management

```cpp
// Create with custom capacity
uint32_t id = gpu.CreateEmitter(desc, 100000);  // 100K particles

// Destroy single emitter
gpu.DestroyEmitter(id);

// Destroy all emitters
gpu.DestroyAllEmitters();
```

### Simulation and Rendering

```cpp
// Each frame
gpu.Update(deltaTime);
gpu.Render(viewMatrix, projMatrix);
```

### Emitter Control

```cpp
gpu.SetEmitterPosition(id, {10.0f, 5.0f, 0.0f});
gpu.SetEmitterPlaying(id, false);  // pause emission

uint32_t count = gpu.GetEmitterCount();
std::string status = gpu.GetStatus();
```

---

## Usage Example

```cpp
auto& gpuParticles = GPUParticleSystem::GetInstance();
gpuParticles.Initialize(device, context);

ParticleEmitterDesc desc;
desc.name = "campfire";
desc.emissionRate = 500.0f;
desc.gravity = {0.0f, 2.0f, 0.0f};  // upward drift
desc.drag = 0.5f;
desc.shapeRadius = 0.3f;

uint32_t fireId = gpuParticles.CreateEmitter(desc, 50000);
gpuParticles.SetEmitterPosition(fireId, {5.0f, 0.0f, 10.0f});

// In render loop
gpuParticles.Update(deltaTime);
gpuParticles.Render(camera.GetViewMatrix(), camera.GetProjectionMatrix());
```

---

## Performance

- **Thread group size**: 256 threads per group (must match HLSL `[numthreads]`)
- **Dispatch count**: `ceil(maxParticles / 256)` groups per emitter per compute pass
- **Sorting**: Bitonic sort is O(n log^2 n) but runs entirely on GPU with high parallelism
- **Memory**: Each particle is 64 bytes (GPUParticleData). 1M particles = ~64 MB per emitter
- **Indirect draw**: Zero CPU readback during normal rendering — the GPU writes draw args directly

---

## Integration

- **GraphicsEngine**: Calls `Update()` and `Render()` as part of the main render pipeline
- **CPU ParticleSystem**: The existing `ParticleSystem` class handles CPU-side particles; GPU particles are a separate, higher-performance path for large counts
- **Platform**: Requires D3D11 compute shader support. Non-Windows platforms get a no-op stub

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) — Overall graphics pipeline
- [GPU-Driven Rendering](GPU-Driven-Rendering.md) — Related GPU compute pipeline for geometry culling
- [Shader Pipeline](../gameplay-tools/Shader-Pipeline.md) — Compute shader compilation
