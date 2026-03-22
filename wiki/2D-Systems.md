# 2D Systems

SparkEngine includes a self-contained 2D subsystem comprising a rigid-body physics world (`Spark::Physics2D`) and a batched sprite renderer (`Spark::Graphics2D`). These systems are designed to operate independently of the 3D Jolt Physics pipeline, making them suitable for pure 2D games, UI overlays, mini-maps, and hybrid 2D/3D scenes. Both subsystems integrate with the EnTT-based Entity Component System and expose dedicated editor panels in SparkEditor.

---

## Table of Contents

1. [Overview](#overview)
2. [Physics2D World](#physics2d-world)
3. [2D Math Types](#2d-math-types)
4. [Collision Shapes](#collision-shapes)
5. [Collision Detection](#collision-detection)
6. [Broadphase (Spatial Hashing)](#broadphase-spatial-hashing)
7. [Collision Resolution](#collision-resolution)
8. [Raycasting](#raycasting)
9. [SpriteBatch Renderer](#spritebatch-renderer)
10. [Blend Modes](#blend-modes)
11. [Nine-Slice Rendering](#nine-slice-rendering)
12. [Debug Drawing](#debug-drawing)
13. [ECS Integration](#ecs-integration)
14. [Editor Panels](#editor-panels)
15. [Testing](#testing)
16. [Console Commands](#console-commands)
17. [Thread Safety](#thread-safety)
18. [See Also](#see-also)

---

## Overview

The 2D subsystem lives in two namespaces:

| Namespace | Header | Purpose |
|---|---|---|
| `Spark::Physics2D` | `SparkEngine/Source/Engine/2D/Physics2D.h` | AABB and circle collision, spatial-hash broadphase, impulse-based resolution, raycasting |
| `Spark::Graphics2D` | `SparkEngine/Source/Graphics/2D/SpriteBatch.h` | Batched sprite rendering with sorting, blend modes, nine-slice, and debug primitives |

Both headers are self-contained, relying only on `Core/Platform.h` (for DirectXMath cross-platform stubs) and the C++ standard library. Neither header pulls in third-party physics or rendering libraries, which keeps compile times low and allows the 2D subsystem to build on Linux without a GPU.

The physics simulation runs during the **Physics** phase of the ECS execution order (Physics -> Animation -> AI -> Audio -> Lifecycle -> Render), and sprite drawing occurs during the **Render** phase.

---

## Physics2D World

`Physics2DWorld` is the central simulation driver. It owns the body list, the spatial hash broadphase, and the contact list. Each frame, the caller invokes `Step(deltaTime)` to advance the simulation by one fixed or variable timestep.

### Simulation Pipeline

The `Step` method executes the following stages in order:

1. **Velocity integration** -- Apply gravity (scaled per body), linear damping, and angular damping to all dynamic bodies. Static and kinematic bodies are skipped.
2. **Broadphase** -- Clear and rebuild the `SpatialHash2D`. Each body's AABB is updated from its position and shape, then inserted into the grid.
3. **Narrowphase** -- For every broadphase candidate pair, perform a layer-mask check, skip static-static pairs, then run the appropriate narrowphase test (AABB-vs-AABB, Circle-vs-Circle, or AABB-vs-Circle).
4. **Collision resolution** -- For non-trigger contacts, apply positional correction and impulse resolution (see [Collision Resolution](#collision-resolution)).
5. **Callback dispatch** -- Fire the user-registered `CollisionCallback` for every contact, including triggers.
6. **Position integration** -- Move all dynamic bodies by their velocity and update rotation (unless `fixedRotation` is set).

### Physics2DWorld API

| Method | Signature | Description |
|---|---|---|
| `SetGravity` | `void SetGravity(float x, float y)` | Set world gravity. Default is `(0, -9.81)`. |
| `GetGravity` | `Vec2 GetGravity() const` | Return the current gravity vector. |
| `SetVelocityIterations` | `void SetVelocityIterations(int iterations)` | Set the number of constraint-solver iterations (default: 8). Higher values improve stacking stability at the cost of CPU time. |
| `Step` | `void Step(float deltaTime)` | Advance the simulation by `deltaTime` seconds. A value of zero or negative is a no-op. |
| `AddBody` | `size_t AddBody(const PhysicsBody2D& body)` | Add a body and return its index. Automatically computes `invMass` and `invInertia` from the body's mass and static/kinematic flags. |
| `Clear` | `void Clear()` | Remove all bodies and contacts. |
| `GetContacts` | `const std::vector<ContactPoint2D>& GetContacts() const` | Return the contact list from the most recent `Step`. |
| `GetBodies` | `std::vector<PhysicsBody2D>& GetBodies()` | Return a mutable reference to the body list. |
| `FindBody` | `PhysicsBody2D* FindBody(uint32_t entityID)` | Linear search for a body by its `entityID`. Returns `nullptr` if not found. |
| `SetCollisionCallback` | `void SetCollisionCallback(CollisionCallback callback)` | Register a callback that fires for every contact (including triggers). |
| `Raycast` | `bool Raycast(const Vec2& origin, const Vec2& direction, float maxDistance, RaycastHit2D& hit, uint32_t layerMask)` | Cast a ray and return the closest hit within `maxDistance`. Respects the layer mask. |

### Usage Example

```cpp
Spark::Physics2D::Physics2DWorld world;
world.SetGravity(0.0f, -9.81f);

// Create a static ground platform
Spark::Physics2D::PhysicsBody2D ground;
ground.entityID = 1;
ground.position = {0.0f, -2.0f};
ground.halfExtents = {10.0f, 0.5f};
ground.isStatic = true;
ground.shapeType = Spark::Physics2D::PhysicsBody2D::ShapeType::Box;
world.AddBody(ground);

// Create a dynamic falling box
Spark::Physics2D::PhysicsBody2D box;
box.entityID = 2;
box.position = {0.0f, 5.0f};
box.halfExtents = {0.5f, 0.5f};
box.mass = 1.0f;
box.restitution = 0.3f;
box.shapeType = Spark::Physics2D::PhysicsBody2D::ShapeType::Box;
world.AddBody(box);

// Register a collision callback
world.SetCollisionCallback([](const Spark::Physics2D::ContactPoint2D& contact)
{
    // Handle collision between contact.bodyA and contact.bodyB
});

// Simulation loop
float fixedDt = 1.0f / 60.0f;
world.Step(fixedDt);
```

---

## 2D Math Types

### Vec2

`Vec2` is a lightweight 2D vector used throughout the Physics2D namespace. It provides the standard arithmetic operators and geometric utilities needed for collision math.

| Member / Method | Description |
|---|---|
| `float x, y` | Component values, default-initialized to `0.0f`. |
| `Vec2(float ax, float ay)` | Construct from scalars. |
| `Vec2(const XMFLOAT2&)` | Construct from DirectXMath `XMFLOAT2`. |
| `operator+`, `operator-`, `operator*` | Element-wise addition, subtraction, and scalar multiplication. |
| `operator+=`, `operator-=`, `operator*=` | In-place compound assignment variants. |
| `Dot(const Vec2&) const` | Dot product: `x*o.x + y*o.y`. |
| `Cross(const Vec2&) const` | 2D cross product (scalar): `x*o.y - y*o.x`. Returns the signed area of the parallelogram formed by the two vectors, useful for winding-order tests. |
| `LengthSq() const` | Squared magnitude. Prefer this over `Length()` when only comparing distances. |
| `Length() const` | Euclidean magnitude via `std::sqrt`. |
| `Normalized() const` | Returns a unit-length copy. If the length is below `1e-6f`, returns `(0, 0)` to avoid division by zero. |
| `ToXMFLOAT2() const` | Convert back to `DirectX::XMFLOAT2` for interop with the 3D rendering pipeline. |

A free function `operator*(float, Vec2)` is also provided so that expressions like `0.5f * velocity` compile correctly.

### AABB2D

An axis-aligned bounding box defined by its `min` and `max` corners.

| Member / Method | Description |
|---|---|
| `Vec2 min, max` | Lower-left and upper-right corners. |
| `Overlaps(const AABB2D&) const` | Returns `true` if this AABB overlaps another (separating-axis test on both axes). |
| `Center() const` | Returns the midpoint `(min + max) * 0.5`. |
| `HalfSize() const` | Returns `(max - min) * 0.5`. |

### ContactPoint2D

Describes a single contact generated during narrowphase collision detection.

| Field | Type | Description |
|---|---|---|
| `point` | `Vec2` | Contact point in world space (midpoint of penetration along the normal). |
| `normal` | `Vec2` | Contact normal directed from body A toward body B. |
| `depth` | `float` | Penetration depth in world units. |
| `bodyA` | `uint32_t` | Entity ID of the first colliding body. |
| `bodyB` | `uint32_t` | Entity ID of the second colliding body. |
| `isTrigger` | `bool` | `true` if either body has `isTrigger` set. Trigger contacts generate callbacks but skip impulse resolution. |

---

## Collision Shapes

Each `PhysicsBody2D` has a `ShapeType` enum that determines which narrowphase test to use:

| ShapeType | Relevant Fields | Description |
|---|---|---|
| `Box` | `halfExtents` (default `{0.5, 0.5}`) | Axis-aligned box centered at `position`. The AABB is computed as `position +/- halfExtents`. |
| `Circle` | `radius` (default `0.5`) | Circle centered at `position`. The AABB is computed as `position +/- radius` on both axes. |

### PhysicsBody2D Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `entityID` | `uint32_t` | `0` | ECS entity this body represents. Used by `FindBody` and reported in contacts. |
| `position` | `Vec2` | `(0, 0)` | World-space center of the body. |
| `rotation` | `float` | `0.0` | Rotation in radians. Updated each step unless `fixedRotation` is true. |
| `velocity` | `Vec2` | `(0, 0)` | Linear velocity in units per second. |
| `angularVelocity` | `float` | `0.0` | Angular velocity in radians per second. |
| `mass` | `float` | `1.0` | Mass in kilograms. Static and kinematic bodies have their `invMass` set to zero. |
| `invMass` | `float` | `1.0` | Reciprocal of mass (computed by `AddBody`). |
| `inertia` | `float` | `1.0` | Moment of inertia (simplified to `mass * 0.5`). |
| `invInertia` | `float` | `1.0` | Reciprocal of inertia (computed by `AddBody`). |
| `friction` | `float` | `0.3` | Coulomb friction coefficient. Combined with the other body via geometric mean. |
| `restitution` | `float` | `0.0` | Bounciness. The minimum restitution of the pair is used during impulse resolution. |
| `linearDamping` | `float` | `0.0` | Drag applied to linear velocity each step. |
| `angularDamping` | `float` | `0.05` | Drag applied to angular velocity each step. |
| `gravityScale` | `float` | `1.0` | Per-body gravity multiplier. Set to `0` for zero-gravity bodies. |
| `isStatic` | `bool` | `false` | Body does not move or respond to forces. |
| `isKinematic` | `bool` | `false` | Body moves only via direct velocity/position manipulation; not affected by gravity or impulses. |
| `fixedRotation` | `bool` | `false` | Prevents angular velocity integration. |
| `isBullet` | `bool` | `false` | Reserved for future continuous collision detection (CCD). Not currently used. |
| `isTrigger` | `bool` | `false` | Generates contacts and callbacks but skips physical response. |
| `layerMask` | `uint32_t` | `0xFFFFFFFF` | Bitmask for collision filtering. Two bodies only collide if `(a.layerMask & b.layerMask) != 0`. |
| `aabb` | `AABB2D` | -- | Automatically computed by the world before each broadphase pass. |
| `shapeType` | `ShapeType` | `Box` | Determines which collision test to use. |
| `halfExtents` | `Vec2` | `(0.5, 0.5)` | Box half-size (only used when `shapeType == Box`). |
| `radius` | `float` | `0.5` | Circle radius (only used when `shapeType == Circle`). |

---

## Collision Detection

Three narrowphase functions are provided as free inline functions in the `Spark::Physics2D` namespace. Each returns `true` on collision and populates `normal` (pointing from A to B) and `depth` (penetration distance).

### TestAABBvsAABB

Tests overlap between two axis-aligned boxes using the separating-axis theorem on both the X and Y axes. The function computes the overlap on each axis and selects the axis of minimum penetration as the contact normal.

```cpp
bool TestAABBvsAABB(const AABB2D& a, const AABB2D& b, Vec2& normal, float& depth);
```

### TestCirclevsCircle

Tests overlap between two circles by comparing the squared distance between their centers against the squared sum of their radii. When the centers are nearly coincident (distance < `1e-6`), a default normal of `(1, 0)` is used to avoid division by zero.

```cpp
bool TestCirclevsCircle(
    const Vec2& posA, float radA,
    const Vec2& posB, float radB,
    Vec2& normal, float& depth);
```

### TestAABBvsCircle

Tests overlap between an AABB and a circle by finding the closest point on the box to the circle center and comparing the distance to the radius. When the circle center is inside the AABB, the function falls back to computing the minimum axis separation to determine the normal direction.

```cpp
bool TestAABBvsCircle(
    const AABB2D& box, const Vec2& circlePos, float circleRadius,
    Vec2& normal, float& depth);
```

---

## Broadphase (Spatial Hashing)

The `SpatialHash2D` class provides an O(1)-average broadphase acceleration structure. It divides the 2D plane into a uniform grid of square cells and maps each cell to a list of body indices.

### How It Works

1. **Clear** -- At the start of each `Step`, the hash map is cleared.
2. **Insert** -- Each body's AABB is converted to grid coordinates (by multiplying by `1 / cellSize` and flooring). The body index is inserted into every cell the AABB touches.
3. **Query** -- Given a query AABB, the same cell-coordinate conversion is performed. All body indices from the overlapping cells are collected, sorted, and deduplicated.

### Configuration

The cell size is set at construction time (default: `2.0` world units). Choosing an appropriate cell size is important for performance:

- **Too large**: Many bodies share the same cells, reducing broadphase effectiveness.
- **Too small**: Large bodies span many cells, increasing insertion and query overhead.
- **Rule of thumb**: Set the cell size to roughly 2x the average body diameter.

### SpatialHash2D API

| Method | Signature | Description |
|---|---|---|
| Constructor | `explicit SpatialHash2D(float cellSize = 2.0f)` | Create a spatial hash with the given cell size. |
| `Clear` | `void Clear()` | Remove all entries. Called at the start of each physics step. |
| `Insert` | `void Insert(uint32_t id, const AABB2D& aabb)` | Insert a body index into all cells overlapped by the AABB. |
| `Query` | `std::vector<uint32_t> Query(const AABB2D& aabb) const` | Return a sorted, deduplicated list of body indices from all cells overlapped by the query AABB. |
| `GetCellSize` | `float GetCellSize() const` | Return the cell size. |

The internal cell key is a 64-bit integer formed by packing the 32-bit X and Y grid coordinates, which avoids the overhead of hashing a pair of integers.

---

## Collision Resolution

When a non-trigger contact is detected, `Physics2DWorld` applies a two-phase resolution:

### 1. Positional Correction

To prevent bodies from sinking into each other over time (the "sinking problem"), a direct positional correction is applied:

- **Slop**: A small penetration allowance (`kSlop = 0.01` units) is permitted before correction kicks in. This prevents jitter from floating-point imprecision.
- **Correction percent**: Only a fraction (`kCorrectionPercent = 0.8`, i.e., 80%) of the excess penetration is corrected each step. This provides smooth convergence.
- **Mass weighting**: The correction is distributed inversely proportional to each body's mass. Static bodies (with `invMass = 0`) receive no correction.

### 2. Impulse Resolution

An instantaneous velocity change is computed along the contact normal:

1. Compute the relative velocity of body B with respect to body A along the contact normal.
2. If the relative velocity is positive (bodies are separating), skip impulse resolution.
3. Compute the impulse scalar `j` using the minimum restitution of the pair: `j = -(1 + e) * velAlongNormal / (invMassA + invMassB)`.
4. Apply the impulse: subtract from A's velocity and add to B's velocity, scaled by their respective inverse masses.

### 3. Friction Impulse

After the normal impulse, a tangential friction impulse is applied:

1. Project the relative velocity onto the contact tangent (perpendicular to the normal).
2. Compute the tangential impulse scalar `jt`.
3. Apply Coulomb's law: if `|jt| < j * mu` (where `mu = sqrt(frictionA * frictionB)`), use the full tangential impulse (static friction). Otherwise, clamp to `j * mu` (dynamic friction).

---

## Raycasting

`Physics2DWorld::Raycast` casts a ray against all bodies in the world and returns the closest hit.

### RaycastHit2D

| Field | Type | Description |
|---|---|---|
| `entityID` | `uint32_t` | Entity ID of the hit body. |
| `point` | `Vec2` | World-space hit point. |
| `normal` | `Vec2` | Approximate surface normal at the hit point (derived from the nearest AABB face). |
| `distance` | `float` | Distance from the ray origin to the hit point. |

### Signature

```cpp
bool Raycast(
    const Vec2& origin,
    const Vec2& direction,
    float maxDistance,
    RaycastHit2D& hit,
    uint32_t layerMask = 0xFFFFFFFF) const;
```

The ray is normalized internally. Bodies whose `layerMask` does not match the query mask are skipped. The implementation uses a slab-based ray-AABB intersection test on both axes and tracks the closest hit.

### Example

```cpp
Spark::Physics2D::Physics2DWorld::RaycastHit2D hit;
if (world.Raycast({0.0f, 0.0f}, {1.0f, 0.0f}, 50.0f, hit))
{
    // hit.entityID, hit.point, hit.normal, hit.distance are populated
}
```

---

## SpriteBatch Renderer

`Spark::Graphics2D::SpriteBatch` implements a batched 2D sprite renderer following the classic Begin/Draw/End pattern. Between `Begin` and `End`, the caller submits any number of sprite draw commands. When `End` is called, the batch sorts all commands, generates vertex data (two triangles per sprite), and merges consecutive sprites that share the same texture and blend mode into a single draw call.

### SpriteBatch API

| Method | Signature | Description |
|---|---|---|
| `Begin` | `void Begin(BlendMode defaultBlend = BlendMode::Alpha)` | Start a new batch frame. Clears all previous commands. |
| `Draw` | `void Draw(const std::string& texturePath, ...)` | Submit a single sprite. Accepts position, size, source UV rect, color tint, rotation, origin, flip flags, sort key, and blend mode. |
| `DrawNineSlice` | `void DrawNineSlice(const std::string& texturePath, ...)` | Submit a nine-slice sprite (see [Nine-Slice Rendering](#nine-slice-rendering)). |
| `DrawLine` | `void DrawLine(const XMFLOAT2& start, const XMFLOAT2& end, ...)` | Draw a line as a thin rotated quad (see [Debug Drawing](#debug-drawing)). |
| `DrawRect` | `void DrawRect(const XMFLOAT2& pos, const XMFLOAT2& size, ...)` | Draw an outlined rectangle using four `DrawLine` calls. |
| `DrawFilledRect` | `void DrawFilledRect(const XMFLOAT2& pos, const XMFLOAT2& size, ...)` | Draw a solid filled rectangle using the `__white__` built-in texture. |
| `End` | `void End()` | Sort commands, generate vertices, and build the draw call list. |
| `GetVertices` | `const std::vector<SpriteVertex>& GetVertices() const` | Return the generated vertex buffer. |
| `GetDrawCalls` | `const std::vector<DrawCall>& GetDrawCalls() const` | Return the draw call list for the GPU renderer to consume. |
| `GetDrawCallCount` | `size_t GetDrawCallCount() const` | Return the number of draw calls. |
| `GetSpriteCount` | `size_t GetSpriteCount() const` | Return the total number of submitted sprites. |
| `IsBatching` | `bool IsBatching() const` | Return `true` if `Begin` has been called but `End` has not. |

### SpriteDrawCommand Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `texturePath` | `std::string` | `""` | Path to the texture asset. The special value `__white__` refers to a built-in 1x1 white texture used for debug primitives. |
| `sortKey` | `int` | `0` | Primary sort key. Lower values are drawn first (back to front). Typically computed as `sortingLayer * 10000 + orderInLayer`. |
| `position` | `XMFLOAT3` | `(0, 0, 0)` | World-space position. The Z component is written into the vertex buffer for depth sorting. |
| `size` | `XMFLOAT2` | `(1, 1)` | Width and height in world units. |
| `origin` | `XMFLOAT2` | `(0.5, 0.5)` | Normalized pivot point. `(0.5, 0.5)` is center; `(0, 0)` is bottom-left. |
| `rotation` | `float` | `0.0` | Rotation in radians around the origin. |
| `sourceRect` | `XMFLOAT4` | `(0, 0, 1, 1)` | UV sub-rectangle `(u0, v0, u1, v1)` for texture atlas support. |
| `color` | `XMFLOAT4` | `(1, 1, 1, 1)` | RGBA tint color multiplied with the texture sample. |
| `flipX` | `bool` | `false` | Mirror the sprite horizontally by swapping U coordinates. |
| `flipY` | `bool` | `false` | Mirror the sprite vertically by swapping V coordinates. |
| `blendMode` | `BlendMode` | `Alpha` | Per-sprite blend mode override. |

### SpriteVertex Layout

Each vertex contains three attributes:

| Attribute | Type | Description |
|---|---|---|
| `position` | `XMFLOAT3` | XY world position plus Z depth. |
| `texCoord` | `XMFLOAT2` | UV coordinates (with flip applied). |
| `color` | `XMFLOAT4` | Per-vertex RGBA tint. |

Each sprite produces 6 vertices (two triangles, no index buffer). The winding order is: bottom-left, bottom-right, top-right (triangle 1) and bottom-left, top-right, top-left (triangle 2).

### Sorting and Batching

When `End()` is called, the command list is sorted using `std::stable_sort` with a two-level comparator:

1. **Primary**: `sortKey` ascending (lower layers drawn first).
2. **Secondary**: `texturePath` lexicographic ascending (groups same-texture sprites together).

After sorting, the vertex generator walks the sorted commands and emits a new `DrawCall` whenever the texture path or blend mode changes. Each `DrawCall` records the texture, the start vertex offset, the vertex count, and the blend mode. The GPU renderer iterates this list, binds the appropriate texture and blend state, and issues one `Draw` call per entry.

### Usage Example

```cpp
Spark::Graphics2D::SpriteBatch batch;

batch.Begin();

// Draw a background sprite at layer 0
batch.Draw("textures/background.png",
           {0.0f, 0.0f, 0.0f},  // position
           {20.0f, 15.0f},       // size
           {0, 0, 1, 1},         // full texture
           {1, 1, 1, 1},         // white tint
           0.0f,                  // no rotation
           {0.5f, 0.5f},         // centered origin
           false, false,          // no flip
           0);                    // sort key

// Draw a player sprite at layer 10
batch.Draw("textures/player.png",
           {5.0f, 3.0f, 0.0f},
           {1.0f, 2.0f},
           {0, 0, 0.25f, 1},     // first frame of a 4-frame atlas
           {1, 1, 1, 1},
           0.0f,
           {0.5f, 0.0f},         // pivot at feet
           false, false,
           10);

batch.End();

// Pass to renderer
const auto& drawCalls = batch.GetDrawCalls();
const auto& vertices = batch.GetVertices();
```

---

## Blend Modes

The `BlendMode` enum controls how each sprite's color is composited onto the framebuffer. The blend mode can be set per-sprite or as a default in `Begin()`.

| Mode | Enum Value | Blend Equation | Typical Use |
|---|---|---|---|
| Alpha | `BlendMode::Alpha` | `src.a, 1 - src.a` | Standard transparency for most sprites. |
| Additive | `BlendMode::Additive` | `src.a, 1` | Glowing effects, particles, light flares. Colors accumulate, never darken. |
| Multiply | `BlendMode::Multiply` | `dst, src` | Shadows, darkening overlays. Multiplies the destination color by the source. |
| Premultiplied Alpha | `BlendMode::PremultipliedAlpha` | `1, 1 - src.a` | Textures with pre-multiplied alpha channels. Avoids dark halos at edges. |

Changing blend modes between sprites within the same batch forces a draw call break, since the GPU blend state must be reconfigured. To minimize draw calls, group sprites by blend mode when possible.

---

## Nine-Slice Rendering

`DrawNineSlice` renders a sprite as a 3x3 grid of quads, keeping the corners at their original size while stretching the edges and center to fill the target rectangle. This is essential for resizable UI elements such as buttons, panels, and dialog boxes.

### Parameters

| Parameter | Type | Description |
|---|---|---|
| `texturePath` | `std::string` | Path to the nine-slice texture. |
| `position` | `XMFLOAT3` | Top-left corner of the output rectangle. |
| `size` | `XMFLOAT2` | Total width and height of the output rectangle. |
| `borderLeft` | `float` | Width of the left border in world units. |
| `borderTop` | `float` | Height of the top border in world units. |
| `borderRight` | `float` | Width of the right border in world units. |
| `borderBottom` | `float` | Height of the bottom border in world units. |
| `color` | `XMFLOAT4` | RGBA tint (default white). |
| `sortKey` | `int` | Sorting layer (default `0`). |
| `fillCenter` | `bool` | Whether to draw the center slice (default `true`). Set to `false` for frames/borders. |

Border values are clamped to a maximum of 49% of the output size to prevent the borders from overlapping. The nine slices are generated as nine individual `Draw` calls, all sharing the same texture and sort key, so they merge into a single draw call.

### Example

```cpp
batch.DrawNineSlice(
    "textures/ui/panel.png",
    {100.0f, 50.0f, 0.0f},   // position
    {400.0f, 300.0f},          // size
    16.0f, 16.0f,              // left, top borders
    16.0f, 16.0f,              // right, bottom borders
    {1, 1, 1, 1},              // white tint
    5,                          // sort key
    true);                      // fill center
```

---

## Debug Drawing

SpriteBatch includes three convenience methods for debug visualization that use the built-in `__white__` texture (a procedural 1x1 white pixel):

### DrawLine

Renders a line between two points as a thin rotated quad. The quad is centered at the midpoint with its width equal to the line length and its height equal to `thickness` (default `0.02` units). The rotation angle is computed via `std::atan2`. Lines default to sort key `9999` to render on top of game sprites.

### DrawRect

Renders an outlined rectangle by issuing four `DrawLine` calls for each edge. Useful for visualizing AABBs, collision regions, and selection boxes.

### DrawFilledRect

Renders a solid colored rectangle. The default alpha is `0.5` for semi-transparent overlays, but this can be overridden via the color parameter.

### Debug Overlay Example

```cpp
batch.Begin();

// Visualize physics AABBs
for (const auto& body : world.GetBodies())
{
    batch.DrawRect(
        {body.aabb.min.x, body.aabb.min.y},
        {body.aabb.max.x - body.aabb.min.x, body.aabb.max.y - body.aabb.min.y},
        {0, 1, 0, 1},  // green outline
        0.02f,
        9999);
}

// Visualize contacts
for (const auto& contact : world.GetContacts())
{
    batch.DrawFilledRect(
        {contact.point.x - 0.05f, contact.point.y - 0.05f},
        {0.1f, 0.1f},
        {1, 0, 0, 1},  // red dot
        10000);

    // Draw contact normal
    batch.DrawLine(
        {contact.point.x, contact.point.y},
        {contact.point.x + contact.normal.x * 0.5f,
         contact.point.y + contact.normal.y * 0.5f},
        {1, 1, 0, 1},  // yellow line
        0.01f,
        10000);
}

batch.End();
```

---

## ECS Integration

The 2D systems integrate with SparkEngine's EnTT-based ECS through dedicated components:

### 2D ECS Components

| Component | Purpose |
|---|---|
| `SpriteRenderer` | Holds texture path, color tint, source rect, pivot, pixels-per-unit, sorting layer, order-in-layer, flip flags, and visibility. Includes a `GetWorldSize()` helper that converts pixel dimensions to world units. |
| `RigidBody2D` | Wraps a `PhysicsBody2D` for the ECS. Stores velocity, mass, damping, gravity scale, and body-type flags. |
| `Collider2D` | Stores shape type, half-extents or radius, trigger flag, layer mask, and material properties (friction, restitution). |
| `Camera2D` | Defines a 2D orthographic camera with position, zoom, rotation, and viewport rect. |
| `SpriteAnimation` | Defines frame-based animation: frame list, frame duration, loop mode, and current playback state. |
| `TilemapRenderer` | References a tileset texture and a 2D grid of tile indices for efficient tilemap rendering. |

### Typical System Loop

The 2D ECS systems follow the engine's execution order. In the Physics phase, the `Physics2DSystem` iterates all entities with both `RigidBody2D` and `Collider2D` components, syncs them into the `Physics2DWorld`, calls `Step`, and writes updated positions back to the `Transform` component. In the Render phase, the `Sprite2DRenderSystem` gathers all `SpriteRenderer` entities, feeds them into a `SpriteBatch`, and submits the resulting vertex data and draw calls to the graphics engine.

```cpp
// Physics phase (simplified)
void Physics2DSystem::Update(float dt, entt::registry& registry)
{
    m_world.Clear();

    auto view = registry.view<Transform2D, RigidBody2D, Collider2D>();
    for (auto entity : view)
    {
        auto& transform = view.get<Transform2D>(entity);
        auto& rb = view.get<RigidBody2D>(entity);
        auto& col = view.get<Collider2D>(entity);

        PhysicsBody2D body;
        body.entityID = static_cast<uint32_t>(entity);
        body.position = {transform.position.x, transform.position.y};
        body.velocity = {rb.velocity.x, rb.velocity.y};
        body.mass = rb.mass;
        body.isStatic = rb.isStatic;
        body.shapeType = col.shapeType;
        body.halfExtents = col.halfExtents;
        body.radius = col.radius;
        m_world.AddBody(body);
    }

    m_world.Step(dt);

    // Write back
    for (const auto& body : m_world.GetBodies())
    {
        auto entity = static_cast<entt::entity>(body.entityID);
        if (registry.valid(entity))
        {
            auto& transform = registry.get<Transform2D>(entity);
            transform.position.x = body.position.x;
            transform.position.y = body.position.y;
            transform.rotation = body.rotation;
        }
    }
}
```

---

## Editor Panels

SparkEditor provides four dedicated panels for working with 2D content:

### Physics2DPanel

Located at `SparkEditor/Source/Panels/Physics2DPanel.h`. Provides:

- **World Settings** -- Gravity vector editor.
- **Collision Layer Matrix** -- A 16x16 matrix editor with named layers (Default, Player, Enemies, Platforms, Projectiles, Triggers, Items, Particles, and 8 custom slots). Toggle which layers collide with each other.
- **Body Inspector** -- Inspect and edit `RigidBody2D` and `Collider2D` properties on the selected entity.
- **Debug Visualization** -- Toggle overlays for AABBs, velocity vectors, contact points, spatial hash grid cells, and collision normals.
- **Performance Stats** -- Live readout of body count, contact count, and step time.
- **Raycast Tool** -- Interactive raycast testing with configurable origin, direction, and max distance.

### SpriteEditorPanel

Located at `SparkEditor/Source/Panels/SpriteEditorPanel.h`. Provides:

- **Texture Preview** -- Zoomable, pannable preview of the sprite texture with optional grid overlay.
- **Source Rect Editor** -- Visual sub-region picker for selecting frames from a texture atlas.
- **Pivot Editor** -- Drag-and-drop pivot point placement with visual indicator.
- **Sorting Settings** -- Sorting layer and order-in-layer configuration.
- **Color Picker** -- RGBA tint color selection.
- **Sprite Properties** -- Pixels-per-unit, flip flags, and visibility toggle.

### SpriteAnimationEditorPanel

Located at `SparkEditor/Source/Panels/SpriteAnimationEditorPanel.h`. Provides a timeline-based editor for authoring frame-by-frame sprite animations, setting frame durations, and configuring loop modes.

### TilemapEditorPanel

Located at `SparkEditor/Source/Panels/TilemapEditorPanel.h`. Provides a visual tile-painting tool for editing `TilemapRenderer` components, with tileset preview, brush selection, and multi-layer support.

---

## Testing

The 2D systems are covered by the `TestSprite2DComponents` test suite located at `Tests/TestSprite2DComponents.cpp`. This file contains **35 test cases** that validate:

- `SpriteRenderer` default values and `GetWorldSize()` computation
- Sprite animation frame advancement and looping
- Tilemap grid indexing
- `Camera2D` viewport calculations
- `RigidBody2D` and `Collider2D` component data integrity
- Physics2D collision detection (AABB-vs-AABB, Circle-vs-Circle, AABB-vs-Circle)
- Spatial hash insertion and query correctness
- Raycast hit detection and distance accuracy

The test file uses minimal re-declarations of DirectXMath types (`TestHelper::Float2`, `Float3`, `Float4`) to avoid the DirectXMath header dependency in CI environments (Linux GCC builds).

Run the tests with:

```bash
cd build && ctest --output-on-failure
# Or directly:
./bin/SparkTests --gtest_filter="*Sprite2D*"
```

---

## Console Commands

The following console commands are available when the 2D systems are active (via `SimpleConsole`):

| Command | Description |
|---|---|
| `physics2d_gravity <x> <y>` | Set the 2D world gravity vector. |
| `physics2d_debug <0|1>` | Toggle 2D physics debug visualization (AABBs, contacts, normals). |
| `physics2d_stats` | Print body count, contact count, and broadphase cell count. |
| `spritebatch_stats` | Print sprite count, draw call count, and vertex count for the current frame. |

---

## Thread Safety

Both 2D systems follow the engine's standard threading model:

- **Physics2DWorld** -- **Main thread only.** The `Step` method and all body mutation methods (`AddBody`, `Clear`, `FindBody`) must be called from the main thread. The body and contact vectors are not protected by any synchronization primitives. This mirrors the 3D `PhysicsSystem` threading contract.

- **SpriteBatch** -- **Main thread only.** The Begin/Draw/End cycle and all vertex generation occurs on the main thread during the render phase. The generated vertex buffer and draw call list are consumed immediately by the GPU submission code on the same thread.

- **Editor panels** -- All four 2D editor panels run on the main thread as part of the ImGui render pass and do not require synchronization.

If you need to perform physics queries from a background thread (e.g., AI pathfinding on a 2D map), copy the relevant body positions into a read-only snapshot before dispatching the background work.

---

## See Also

- [Physics](Physics) -- 3D Jolt Physics integration and the `PhysicsSystem` ECS system
- [Rendering and Graphics](Rendering-and-Graphics) -- DX11 graphics engine, shader pipeline, and 3D rendering
- [Entity Component System](Entity-Component-System) -- EnTT-based ECS architecture, component registration, and system execution order
- [SparkEditor](SparkEditor) -- Editor panel architecture and the 22 subsystem panels
- [Animation](Animation) -- Skeletal animation, IK, and state machines (3D counterpart to sprite animation)
- [Testing](Testing) -- Test framework, CTest configuration, and test coverage
