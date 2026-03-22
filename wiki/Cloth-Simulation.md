# Cloth Simulation

SparkEngine provides a position-based dynamics (PBD) cloth simulator for flags, capes, curtains, and other deformable surfaces. The system supports structural, shear, and bending constraints, pin constraints, wind and gravity forces, collision with simple shapes, and an optional soft body system for volumetric deformable objects.

**Source:** `SparkEngine/Source/Physics/ClothSimulation.h`

## Architecture Overview

The cloth simulation uses a particle-constraint model where a grid of mass points (particles) is connected by distance constraints. Each simulation step applies external forces, integrates particle positions using Verlet integration, iteratively solves constraints to maintain structural integrity, and resolves collisions with the environment.

```
+-------------------+
| ClothSimulation   |  Owns and manages all cloth instances
|   Update(dt)      |
+--------+----------+
         |
         | for each enabled instance:
         v
+-------------------+     +-------------------+
| ClothInstance #1  |     | ClothInstance #2  |
|   particles[]     |     |   particles[]     |
|   constraints[]   |     |   constraints[]   |
|   colliders[]     |     |   colliders[]     |
+--------+----------+     +-------------------+
         |
         | SimulateCloth(dt)
         v
+-------------------------------------------+
| 1. ApplyForces()      — gravity + wind    |
| 2. IntegratePositions() — Verlet step     |
| 3. SolveConstraints()  — N iterations     |
| 4. HandleCollisions()  — sphere/plane     |
+-------------------------------------------+
```

### Simulation Pipeline Per Frame

```
                    deltaTime
                       |
                       v
            +----------+----------+
            |    ApplyForces()    |
            |  gravity + wind    |
            |  -> acceleration   |
            +----------+----------+
                       |
                       v
            +----------+----------+
            | IntegratePositions()|
            |  Verlet integration |
            |  position += vel*dt |
            |       + accel*dt^2  |
            +----------+----------+
                       |
                       v
            +----------+----------+
            | SolveConstraints()  |  <---+
            |  distance correction|      | repeat N times
            |  for each constraint|      | (solverIterations)
            +----------+----------+  ----+
                       |
                       v
            +----------+----------+
            | HandleCollisions()  |
            |  sphere, capsule,   |
            |  plane collision    |
            +----------+----------+
                       |
                       v
              Particles ready for
              rendering upload
```

## Namespace and Header

```cpp
#include "Physics/ClothSimulation.h"

// All types live in the Spark::Physics namespace
using namespace Spark::Physics;
```

Required headers pulled in by `ClothSimulation.h`:

| Header | Purpose |
|--------|---------|
| `Core/Platform.h` | Cross-platform math type stubs |
| `<DirectXMath.h>` | `XMFLOAT3`, `XMFLOAT4` (Windows only) |
| `<string>` | Console status output |
| `<vector>` | Particle, constraint, and collider storage |
| `<unordered_map>` | Instance lookup by ID |
| `<cstdint>` | `uint32_t` for particle indices and cloth IDs |

## Class and Struct Reference

### ClothParticle

A single point mass in the cloth simulation grid.

```cpp
struct ClothParticle
{
    DirectX::XMFLOAT3 position{0, 0, 0};     ///< Current world position
    DirectX::XMFLOAT3 prevPosition{0, 0, 0}; ///< Previous frame position (for Verlet integration)
    DirectX::XMFLOAT3 velocity{0, 0, 0};     ///< Current velocity (derived from position delta)
    DirectX::XMFLOAT3 acceleration{0, 0, 0}; ///< Accumulated forces divided by mass
    float inverseMass = 1.0f;                ///< Inverse mass (0 = pinned/infinite mass)
    bool pinned = false;                     ///< Whether this particle is pinned to a fixed position
};
```

**Field Details:**

| Field | Type | Description |
|-------|------|-------------|
| `position` | `XMFLOAT3` | Current world-space position. Updated each frame by integration and constraint solving. |
| `prevPosition` | `XMFLOAT3` | Position from the previous frame. Used by Verlet integration to implicitly encode velocity. |
| `velocity` | `XMFLOAT3` | Derived velocity computed after integration for external queries (rendering, audio). Not used internally by the integrator. |
| `acceleration` | `XMFLOAT3` | Sum of all forces (gravity, wind) divided by particle mass. Reset and recomputed each frame in `ApplyForces()`. |
| `inverseMass` | `float` | Reciprocal of the particle mass. Set to 0 for pinned particles (effectively infinite mass). Used during constraint solving to distribute corrections proportionally. |
| `pinned` | `bool` | When `true`, the particle's position is fixed and unaffected by forces, integration, or constraints. |

### ClothConstraint

A distance constraint between two particles that maintains their rest distance.

```cpp
struct ClothConstraint
{
    uint32_t particleA = 0;  ///< Index of the first particle
    uint32_t particleB = 0;  ///< Index of the second particle
    float restLength = 0.0f; ///< Target distance between particles
    float stiffness = 1.0f;  ///< Constraint stiffness (0.0 = no correction, 1.0 = full correction)
};
```

The constraint solver computes the current distance between `particleA` and `particleB`, then pushes or pulls them toward the `restLength`. The `stiffness` parameter controls how much of the error is corrected each iteration. Multiple solver iterations with stiffness < 1.0 converge to a stable solution while allowing some elasticity.

**Constraint types created during cloth initialization:**

| Type | Connection Pattern | Rest Length | Stiffness |
|------|-------------------|-------------|-----------|
| Structural (horizontal) | `(x, y)` to `(x+1, y)` | `spacing` | `desc.stiffness` |
| Structural (vertical) | `(x, y)` to `(x, y+1)` | `spacing` | `desc.stiffness` |
| Shear (diagonal) | `(x, y)` to `(x+1, y+1)` | `spacing * 1.414` | `desc.stiffness * 0.8` |
| Bending (horizontal) | `(x, y)` to `(x+2, y)` | `spacing * 2.0` | `desc.bendStiffness` |
| Bending (vertical) | `(x, y)` to `(x, y+2)` | `spacing * 2.0` | `desc.bendStiffness` |

```
Constraint Layout for a 4x4 Grid:

  P---P---P---P       P = particle
  |\ /|\ /|\ /|       - = structural (horizontal)
  | X | X | X |       | = structural (vertical)
  |/ \|/ \|/ \|       \ / = shear (diagonal)
  P---P---P---P       Bending constraints skip one particle
  |\ /|\ /|\ /|       (not shown for clarity)
  | X | X | X |
  |/ \|/ \|/ \|
  P---P---P---P
  |\ /|\ /|\ /|
  | X | X | X |
  |/ \|/ \|/ \|
  P---P---P---P
```

### ClothCollider

A simple shape used for cloth-to-environment collision detection.

```cpp
struct ClothCollider
{
    enum class Type
    {
        Sphere,   ///< Spherical collider
        Capsule,  ///< Capsule collider (sphere-swept line)
        Plane     ///< Infinite half-plane collider
    };

    Type type = Type::Sphere;
    DirectX::XMFLOAT3 position{0, 0, 0};  ///< Center position (sphere/capsule) or point on plane
    DirectX::XMFLOAT3 normal{0, 1, 0};    ///< Normal for plane; axis direction for capsule
    float radius = 0.5f;                   ///< Radius for sphere/capsule
    float height = 1.0f;                   ///< Total height for capsule
};
```

**Collider Type Details:**

| Type | `position` Meaning | `normal` Meaning | `radius` | `height` |
|------|-------------------|------------------|----------|----------|
| `Sphere` | Center of sphere | Unused | Sphere radius | Unused |
| `Capsule` | Center of capsule | Capsule axis direction (unit vector) | Capsule radius | Total capsule height |
| `Plane` | Any point on the plane | Plane normal (points toward valid side) | Unused | Unused |

**Collision Response:**
- **Sphere:** If a particle is inside the sphere, it is projected onto the sphere surface along the vector from the sphere center to the particle.
- **Plane:** If a particle is on the negative side of the plane (dot product with normal < 0), it is projected onto the plane surface.
- **Capsule:** Treated as the union of a cylinder and two hemisphere end caps.

### ClothDescriptor

Configuration parameters for creating a new cloth instance.

```cpp
struct ClothDescriptor
{
    int width = 10;                     ///< Particles in X direction
    int height = 10;                    ///< Particles in Y direction
    float spacing = 0.1f;              ///< Distance between adjacent particles (meters)
    float mass = 1.0f;                 ///< Total cloth mass (kg)
    float stiffness = 0.9f;            ///< Structural constraint stiffness (0.0-1.0)
    float bendStiffness = 0.5f;        ///< Bending constraint stiffness (0.0-1.0)
    float damping = 0.01f;             ///< Velocity damping factor (0.0-1.0)
    DirectX::XMFLOAT3 origin{0, 0, 0}; ///< World-space origin of the cloth grid
    int solverIterations = 4;          ///< Constraint solver iterations per frame
};
```

**Parameter Guide:**

| Field | Default | Range | Effect of Increasing |
|-------|---------|-------|---------------------|
| `width` | 10 | 2-100 | More particles horizontally; higher detail but more CPU cost |
| `height` | 10 | 2-100 | More particles vertically; higher detail but more CPU cost |
| `spacing` | 0.1 | 0.01-1.0 | Larger cloth area; particles farther apart |
| `mass` | 1.0 | 0.01-100 | Heavier cloth; sags more under gravity, less affected by wind |
| `stiffness` | 0.9 | 0.0-1.0 | Stiffer cloth; maintains shape better, less stretchy |
| `bendStiffness` | 0.5 | 0.0-1.0 | More resistance to bending; cloth appears thicker/stiffer |
| `damping` | 0.01 | 0.0-0.5 | More energy loss; cloth settles faster, less oscillation |
| `solverIterations` | 4 | 1-32 | More accurate constraint solving; stiffer appearance |

### ClothInstance

Runtime state of a single cloth simulation, created by `CreateCloth()`.

```cpp
struct ClothInstance
{
    uint32_t id = 0;                              ///< Unique instance handle
    std::vector<ClothParticle> particles;          ///< All particles in the cloth grid
    std::vector<ClothConstraint> constraints;      ///< All constraints (structural, shear, bending)
    std::vector<ClothCollider> colliders;           ///< Colliders for this cloth instance
    DirectX::XMFLOAT3 gravity{0, -9.81f, 0};     ///< Gravity vector
    DirectX::XMFLOAT3 wind{0, 0, 0};             ///< Wind force vector
    float damping = 0.01f;                        ///< Velocity damping
    int solverIterations = 4;                     ///< Constraint iterations per step
    int width = 0;                                ///< Grid width (particles)
    int height = 0;                               ///< Grid height (particles)
    bool enabled = true;                          ///< Whether simulation is active
};
```

**Particle indexing:** Particles are stored in row-major order. The particle at grid position `(x, y)` has index `y * width + x`. This is important when using `PinParticle()` and `UnpinParticle()`.

### SoftBodyDescriptor

Configuration for creating a volumetric soft body (tetrahedral mesh).

```cpp
struct SoftBodyDescriptor
{
    std::vector<DirectX::XMFLOAT3> vertices; ///< Initial vertex positions
    std::vector<uint32_t> tetrahedra;        ///< Tetrahedral mesh connectivity (4 indices per tet)
    float mass = 5.0f;                       ///< Total body mass (kg)
    float stiffness = 0.8f;                  ///< Distance constraint stiffness
    float volumeStiffness = 0.9f;            ///< Volume preservation constraint strength
    float damping = 0.02f;                   ///< Velocity damping
    int solverIterations = 8;                ///< Constraint iterations per step
};
```

Soft bodies use the same PBD framework as cloth but add volume preservation constraints that prevent the tetrahedral mesh from collapsing.

## ClothSimulation Class API

```cpp
class ClothSimulation
{
public:
    ClothSimulation();
    ~ClothSimulation() = default;

    // --- Lifecycle ---
    uint32_t CreateCloth(const ClothDescriptor& desc);
    void DestroyCloth(uint32_t clothId);

    // --- Particle control ---
    void PinParticle(uint32_t clothId, uint32_t particleIndex, const DirectX::XMFLOAT3& position);
    void UnpinParticle(uint32_t clothId, uint32_t particleIndex);

    // --- Forces ---
    void SetWind(uint32_t clothId, const DirectX::XMFLOAT3& wind);

    // --- Colliders ---
    void AddCollider(uint32_t clothId, const ClothCollider& collider);

    // --- Simulation ---
    void Update(float deltaTime);

    // --- Queries ---
    const std::vector<ClothParticle>& GetParticles(uint32_t clothId) const;
    void GetClothDimensions(uint32_t clothId, int& width, int& height) const;
    size_t GetInstanceCount() const;

    // --- Console ---
    std::string Console_GetStatus() const;
};
```

**Method Reference:**

| Method | Return | Description |
|--------|--------|-------------|
| `CreateCloth(desc)` | `uint32_t` | Creates a new cloth from the descriptor. Returns a handle for all subsequent operations. Particles are laid out in a flat grid at `desc.origin`. |
| `DestroyCloth(id)` | `void` | Removes and destroys a cloth instance. The handle becomes invalid. |
| `PinParticle(id, idx, pos)` | `void` | Pins a particle to a fixed world position. Sets `inverseMass = 0` and `pinned = true`. The particle will not move during simulation. |
| `UnpinParticle(id, idx)` | `void` | Unpins a particle, restoring `inverseMass = 1.0` and allowing it to move freely. |
| `SetWind(id, wind)` | `void` | Sets the wind force vector for a cloth instance. Wind is applied as a constant acceleration to all non-pinned particles. |
| `AddCollider(id, collider)` | `void` | Adds a collision shape to a cloth instance. Multiple colliders can be added to a single cloth. |
| `Update(deltaTime)` | `void` | Steps all enabled cloth instances forward by `deltaTime` seconds. Calls `SimulateCloth()` for each. |
| `GetParticles(id)` | `const vector<ClothParticle>&` | Returns a read-only reference to the particle array for rendering. Returns an empty static vector if the ID is invalid. |
| `GetClothDimensions(id, w, h)` | `void` | Outputs the grid dimensions. Sets both to 0 if the ID is invalid. |
| `GetInstanceCount()` | `size_t` | Returns the number of active cloth instances. |
| `Console_GetStatus()` | `std::string` | Returns a formatted string listing all instances with particle/constraint/collider counts. |

### Private Simulation Methods

These methods implement the core PBD simulation loop:

| Method | Description |
|--------|-------------|
| `SimulateCloth(cloth, dt)` | Orchestrates the full simulation step: forces, integration, constraints, collisions. |
| `ApplyForces(cloth, dt)` | Computes `acceleration = gravity + wind` for each non-pinned particle. |
| `IntegratePositions(cloth, dt)` | Verlet integration: `newPos = pos + (pos - prevPos) * dampFactor + accel * dt^2`. Also derives velocity for external queries. |
| `SolveConstraints(cloth)` | Iterates over all constraints, correcting particle positions to satisfy rest lengths. Corrections are weighted by inverse mass ratios. |
| `HandleCollisions(cloth)` | Projects particles out of collider volumes (sphere surface projection, plane half-space projection). |

## Quick Start

```cpp
ClothSimulation cloth;

// Create a 10x10 particle cloth (100 particles, ~490 constraints)
ClothDescriptor desc;
desc.width = 10;
desc.height = 10;
desc.spacing = 0.1f;
desc.mass = 0.5f;
desc.stiffness = 0.9f;
desc.bendStiffness = 0.5f;
desc.damping = 0.01f;
desc.solverIterations = 4;

auto id = cloth.CreateCloth(desc);

// Pin top-left and top-right corners (row 0, columns 0 and 9)
cloth.PinParticle(id, 0, {0, 2, 0});       // particle (0,0)
cloth.PinParticle(id, 9, {0.9f, 2, 0});    // particle (9,0)

// Apply wind blowing in the +X direction
cloth.SetWind(id, {1.0f, 0, 0.5f});

// Per frame in your game loop:
cloth.Update(deltaTime);

// Get particles for rendering
const auto& particles = cloth.GetParticles(id);
// Upload particle positions to a dynamic vertex buffer
```

## Particle Indexing and Pin Patterns

Particles are stored in row-major order: `index = y * width + x`.

```
Grid layout for width=5, height=4:

  Row 0:  [0]  [1]  [2]  [3]  [4]      <- top row
  Row 1:  [5]  [6]  [7]  [8]  [9]
  Row 2:  [10] [11] [12] [13] [14]
  Row 3:  [15] [16] [17] [18] [19]      <- bottom row
```

### Common Pin Patterns

**Flag (pinned along left edge):**
```cpp
for (int y = 0; y < desc.height; ++y)
{
    uint32_t idx = static_cast<uint32_t>(y * desc.width);
    float yPos = 2.0f - static_cast<float>(y) * desc.spacing;
    cloth.PinParticle(id, idx, {0, yPos, 0});
}
```

**Curtain (pinned along top edge):**
```cpp
for (int x = 0; x < desc.width; ++x)
{
    float xPos = static_cast<float>(x) * desc.spacing;
    cloth.PinParticle(id, static_cast<uint32_t>(x), {xPos, 3.0f, 0});
}
```

**Cape (pinned at two shoulder points):**
```cpp
cloth.PinParticle(id, 0, shoulderLeft);
cloth.PinParticle(id, static_cast<uint32_t>(desc.width - 1), shoulderRight);
```

**Tablecloth (pinned at four corners):**
```cpp
uint32_t w = static_cast<uint32_t>(desc.width);
uint32_t h = static_cast<uint32_t>(desc.height);
cloth.PinParticle(id, 0, cornerTL);                           // top-left
cloth.PinParticle(id, w - 1, cornerTR);                       // top-right
cloth.PinParticle(id, (h - 1) * w, cornerBL);                 // bottom-left
cloth.PinParticle(id, (h - 1) * w + w - 1, cornerBR);         // bottom-right
```

## Colliders

Add simple colliders to prevent cloth from passing through objects:

```cpp
// Sphere collider (e.g., a ball the cloth drapes over)
ClothCollider sphere;
sphere.type = ClothCollider::Type::Sphere;
sphere.position = {0, 1, 0};
sphere.radius = 0.3f;
cloth.AddCollider(id, sphere);

// Ground plane
ClothCollider ground;
ground.type = ClothCollider::Type::Plane;
ground.position = {0, 0, 0};
ground.normal = {0, 1, 0};
cloth.AddCollider(id, ground);

// Capsule collider (e.g., a character limb)
ClothCollider capsule;
capsule.type = ClothCollider::Type::Capsule;
capsule.position = {0.5f, 1.0f, 0};
capsule.normal = {0, 1, 0};    // Capsule axis direction
capsule.radius = 0.1f;
capsule.height = 0.6f;
cloth.AddCollider(id, capsule);
```

### Collision Response Details

The collision handler runs after constraint solving. For each collider, every non-pinned particle is tested:

**Sphere collision:**
```
if distance(particle, sphere.center) < sphere.radius:
    push particle to sphere surface along the center-to-particle direction
```

**Plane collision:**
```
signed_distance = dot(particle.pos - plane.pos, plane.normal)
if signed_distance < 0:
    particle.pos -= plane.normal * signed_distance
```

Colliders do not apply friction or restitution in the current implementation. Particles that penetrate a collider are simply projected to the collider surface.

## Verlet Integration Details

The cloth simulation uses Verlet integration rather than Euler integration because it is more stable for constraint-based systems and implicitly handles velocity.

**Verlet integration formula:**

```
newPosition = position + (position - prevPosition) * dampFactor + acceleration * dt^2
```

Where `dampFactor = 1.0 - damping`. The damping term removes a fraction of the implicit velocity each frame, causing the cloth to settle over time rather than oscillating indefinitely.

**Advantages over Euler integration:**
- Velocity is implicit (no separate velocity integration step)
- More stable under large time steps
- Constraint corrections automatically affect velocity
- Simpler code with fewer parameters

**Velocity derivation:**
After integration, velocity is computed as `(position - prevPosition) / dt` for external systems that need it (audio wind effects, rendering motion blur).

## Constraint Solver Details

The constraint solver uses Gauss-Seidel relaxation: it iterates over all constraints sequentially, correcting particle positions to satisfy each constraint's rest length.

**Correction formula for a single constraint:**

```
dx = particleB.position - particleA.position
distance = length(dx)
correction = (distance - restLength) / distance * stiffness
ratioA = inverseMassA / (inverseMassA + inverseMassB)
ratioB = inverseMassB / (inverseMassA + inverseMassB)

particleA.position += dx * correction * ratioA   (if not pinned)
particleB.position -= dx * correction * ratioB   (if not pinned)
```

The inverse mass ratio ensures that lighter particles move more than heavier ones. Pinned particles (inverseMass = 0) do not move at all, and the entire correction is applied to the other particle.

**Convergence:** With `stiffness = 1.0`, each constraint is fully satisfied in one iteration, but interacting constraints may conflict. Multiple solver iterations allow the system to converge to a globally consistent state.

| Iterations | Behavior |
|-----------|----------|
| 1 | Very stretchy cloth; constraints barely enforced |
| 2-4 | Soft, elastic cloth; good for capes and curtains |
| 8-16 | Stiff cloth; good for canvas and rigid fabric |
| 32+ | Nearly rigid; diminishing returns on quality |

## Performance Notes

### Complexity Analysis

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Force application | O(N) | N = particle count |
| Verlet integration | O(N) | N = particle count |
| Constraint solving | O(C * I) | C = constraint count, I = solver iterations |
| Collision detection | O(N * K) | N = particles, K = colliders |
| **Total per frame** | **O(N * K + C * I)** | Dominated by constraints and collisions |

### Constraint Count Estimates

For a `W x H` cloth grid:

| Constraint Type | Count |
|----------------|-------|
| Structural (horiz) | `(W-1) * H` |
| Structural (vert) | `W * (H-1)` |
| Shear (diagonal) | `(W-1) * (H-1)` |
| Bending (horiz) | `(W-2) * H` |
| Bending (vert) | `W * (H-2)` |
| **Total** | Approximately `5*W*H` |

**Example:** A 20x20 cloth has 400 particles and roughly 2,000 constraints. With 4 solver iterations, that is 8,000 constraint evaluations per frame.

### Optimization Recommendations

| Concern | Recommendation |
|---------|---------------|
| Distant cloth appears identical to high-res | Reduce `width`/`height` for distant instances (LOD) |
| Too many solver iterations | Use 2-4 iterations for most cloth; reserve 8+ for hero cloth only |
| Many colliders per cloth | Keep collider count under 5; use simplified shapes |
| Self-collision needed | Avoid if possible; it is O(N^2) without spatial hashing |
| Many cloth instances | Disable simulation for off-screen cloth (`enabled = false`) |
| Large time steps cause instability | Subdivide deltaTime: `cloth.Update(deltaTime / 2); cloth.Update(deltaTime / 2);` |

### Memory Usage Estimate

| Component | Size Per Element | Count (20x20 cloth) | Total |
|-----------|-----------------|---------------------|-------|
| ClothParticle | ~64 bytes | 400 | ~25 KB |
| ClothConstraint | ~16 bytes | ~2,000 | ~32 KB |
| ClothCollider | ~40 bytes | ~3 | ~120 bytes |
| **Total per instance** | | | **~57 KB** |

## Integration with Rendering

After `Update()`, particle positions need to be uploaded to the GPU for rendering. The typical pattern:

```cpp
// Get particle data
const auto& particles = cloth.GetParticles(clothId);
int w, h;
cloth.GetClothDimensions(clothId, w, h);

// Build vertex buffer
std::vector<Vertex> vertices(particles.size());
for (size_t i = 0; i < particles.size(); ++i)
{
    vertices[i].position = particles[i].position;
    // Compute UVs from grid position
    int x = static_cast<int>(i) % w;
    int y = static_cast<int>(i) / w;
    vertices[i].uv.x = static_cast<float>(x) / static_cast<float>(w - 1);
    vertices[i].uv.y = static_cast<float>(y) / static_cast<float>(h - 1);
}

// Build index buffer (triangle grid)
std::vector<uint32_t> indices;
for (int y = 0; y < h - 1; ++y)
{
    for (int x = 0; x < w - 1; ++x)
    {
        uint32_t tl = static_cast<uint32_t>(y * w + x);
        uint32_t tr = tl + 1;
        uint32_t bl = tl + static_cast<uint32_t>(w);
        uint32_t br = bl + 1;
        // Two triangles per quad
        indices.insert(indices.end(), {tl, bl, tr, tr, bl, br});
    }
}

// Upload to dynamic vertex buffer each frame
UpdateDynamicBuffer(clothVertexBuffer, vertices);
```

## Integration with ECS

To use cloth with the entity component system, attach cloth to entities:

```cpp
// Create a cloth component
struct ClothComponent
{
    uint32_t clothId = 0;
    uint32_t meshEntity = 0;  // Entity with the MeshComponent to update
};

// In your cloth system update:
void ClothECSSystem::Update(float dt, ClothSimulation& clothSim, entt::registry& registry)
{
    clothSim.Update(dt);

    auto view = registry.view<ClothComponent, TransformComponent>();
    for (auto [entity, clothComp, transform] : view.each())
    {
        // Update pinned particles to follow the entity's transform
        // Upload particles to the associated mesh
    }
}
```

## Console Commands

```
cloth_status    # Show active cloth instances and particle counts
cloth_wind      # Show/set global wind direction
cloth_debug     # Toggle cloth debug visualization (wireframe, constraints, colliders)
```

### Console Output Example

```
=== Cloth Simulation ===
Instances: 3
  Cloth 1: 100 particles, 490 constraints, 2 colliders
  Cloth 2: 400 particles, 1960 constraints, 1 colliders
  Cloth 3: 25 particles, 110 constraints, 0 colliders [DISABLED]
```

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Cloth falls through ground | No plane collider added | Add a `ClothCollider::Type::Plane` at ground level |
| Cloth stretches excessively | Stiffness too low or too few solver iterations | Increase `stiffness` to 0.95+ or increase `solverIterations` to 8+ |
| Cloth explodes / particles fly off | `deltaTime` too large (> 33ms) or `damping` too low | Subdivide time step; increase `damping` to 0.05+ |
| Cloth does not respond to wind | Wind vector is zero or particles are all pinned | Check `SetWind()` was called with non-zero vector; verify not all particles are pinned |
| `GetParticles()` returns empty vector | Invalid cloth ID | Verify `CreateCloth()` returned a valid handle; check the ID was not destroyed |
| Cloth appears as a flat plane | No pins set; cloth falls instantly under gravity | Pin at least one particle before the first `Update()` call |
| Cloth tunnels through sphere collider | Particle velocity too high relative to collider radius | Reduce time step, increase damping, or use larger collider radius |
| Performance is poor with many instances | Too many particles or solver iterations | Use LOD (lower resolution for distant cloth); disable off-screen instances |

---

## See Also

- [Physics](Physics) -- Rigid body simulation with Jolt Physics
- [Animation](Animation) -- Character capes and cloth attachments
- [Rendering and Graphics](Rendering-and-Graphics) -- Rendering deformable meshes
- [Entity Component System](Entity-Component-System) -- Integrating cloth with ECS
