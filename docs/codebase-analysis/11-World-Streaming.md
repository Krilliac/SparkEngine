# 11 — World & Streaming Systems

**Locations:**
- `SparkEngine/Source/Engine/World/` — Origin rebasing, spatial grid, day/night, triggers
- `SparkEngine/Source/Engine/Streaming/` — Area streaming

---

## WorldOriginSystem — Large-World Support

**File:** `SparkEngine/Source/Engine/World/WorldOriginSystem.h`

Prevents floating-point precision loss when players move far from the world origin by rebasing all coordinates.

### How It Works

```cpp
WorldOriginSystem& origin = WorldOriginSystem::GetInstance();
origin.Initialize();
origin.SetRebaseThreshold(5000.0f);  // Rebase when player > 5000 units from origin

// Each frame
origin.Update(playerPosition);

// If player exceeds threshold:
// 1. Compute rebase offset (player position snapped to grid)
// 2. Shift ALL objects by -offset
// 3. Fire callbacks for subsystem notification
```

### Rebase Callbacks

```cpp
origin.RegisterRebaseCallback("Physics", [](const XMFLOAT3& offset) {
    physics->ShiftOrigin(offset);
});
origin.RegisterRebaseCallback("Audio", [](const XMFLOAT3& offset) {
    audio->ShiftListenerPosition(offset);
});
origin.RegisterRebaseCallback("Particles", [](const XMFLOAT3& offset) {
    particles->ShiftAllParticles(offset);
});
origin.RegisterRebaseCallback("NavMesh", [](const XMFLOAT3& offset) {
    navMesh->ShiftNavMesh(offset);
});
```

### State Tracking

```cpp
XMFLOAT3 accumulated = origin.GetAccumulatedOffset();
int rebaseCount = origin.GetRebaseCount();
float peakDistance = origin.GetPeakDistance();
```

---

## SpatialGrid — Cell-Based Partitioning

**File:** `SparkEngine/Source/Engine/World/SpatialGrid.h`

TrinityCore-inspired fixed-size cell grid for efficient spatial queries:

### Cell Architecture

```cpp
SpatialGrid grid;
grid.Initialize(64.0f);  // 64-unit cell size

// Cell states
enum class CellState { Active, Idle, Unloaded };
// Active: fully simulated
// Idle: reduced update rate
// Unloaded: not tracked (default unload delay: 300s)
```

### Operations

```cpp
// Insert/remove entities
grid.AddEntity(entityId, position);
grid.RemoveEntity(entityId);
grid.MoveEntity(entityId, newPosition);

// Spatial queries (O(1) cell lookup)
auto nearby = grid.GetEntitiesInRadius(center, radius);
auto cellEntities = grid.GetEntitiesInCell(cellX, cellZ);

// Visit nearby cells
grid.VisitNearbyCells(center, radius, [](const Cell& cell) {
    for (auto entityId : cell.entities) {
        // Process entity
    }
});
```

---

## SeamlessAreaManager — Predictive Streaming

**File:** `SparkEngine/Source/Engine/Streaming/SeamlessAreaManager.h`

Predictive asset streaming that preloads areas before the player reaches them:

### Configuration

```cpp
SeamlessAreaManager streaming;
streaming.Initialize();

AreaDefinition area;
area.name = "Forest";
area.boundsMin = {0, 0, 0};
area.boundsMax = {500, 100, 500};
area.priority = 1;
streaming.RegisterArea(area);

streaming.SetLoadRadius(500.0f);     // Load when player within 500m
streaming.SetUnloadRadius(800.0f);   // Unload when player beyond 800m
streaming.SetMaxConcurrentLoads(2);  // Max 2 areas loading simultaneously
streaming.SetLookaheadTime(3.0f);    // Predict 3s of player movement
```

### Area States

```
Unloaded → Loading → Loaded → Unloading → Unloaded
```

### Per-Frame Update

```cpp
streaming.SetPlayerState(position, velocity, cameraForward);
streaming.Update(deltaTime);
// Internally:
// 1. Predict future player position (position + velocity * lookahead)
// 2. Check each area against load/unload radius
// 3. Begin loading nearby areas, unload distant ones
// 4. Respect max concurrent load limit
```

### Thread Safety

`SetPlayerState()` is thread-safe; other methods are main-thread only.

---

## TimeOfDaySystem — Day/Night Cycle

**File:** `SparkEngine/Source/Engine/World/TimeOfDaySystem.h`

24-hour game clock with derived lighting:

```cpp
TimeOfDaySystem& tod = TimeOfDaySystem::GetInstance();
tod.Initialize();
tod.SetTimeScale(60.0f);  // 1 game minute = 1 real second
tod.SetCurrentHour(14.0f);  // 2:00 PM

tod.Update(deltaTime);

float hour = tod.GetCurrentHour();       // [0, 24)
int dayCount = tod.GetDayCount();
DayPeriod period = tod.GetCurrentPeriod();

// Derived values
XMFLOAT3 sunDir = tod.GetSunDirection();
XMFLOAT3 sunColor = tod.GetSunColor();    // Warm at dawn/dusk, white at noon
float sunIntensity = tod.GetSunIntensity();
XMFLOAT3 ambient = tod.GetAmbientColor();
```

### Day Periods

| Period | Hours | Description |
|--------|-------|-------------|
| Night | 0:00 - 5:00 | Dark, blue tint |
| Dawn | 5:00 - 7:00 | Warm orange/pink |
| Morning | 7:00 - 10:00 | Brightening |
| Midday | 10:00 - 14:00 | Full brightness, white |
| Afternoon | 14:00 - 17:00 | Slight warming |
| Dusk | 17:00 - 19:00 | Orange/red sunset |
| Evening | 19:00 - 21:00 | Darkening |
| LateNight | 21:00 - 0:00 | Dark, blue tint |

---

## ProximityTriggerSystem — Spatial Triggers

**File:** `SparkEngine/Source/Engine/World/ProximityTriggerSystem.h`

Spatial trigger volumes for gameplay events:

```cpp
ProximityTriggerSystem triggers;

// Register trigger
TriggerVolume trigger;
trigger.shape = TriggerShape::Sphere;
trigger.center = {100, 0, 50};
trigger.radius = 10.0f;
trigger.onEnter = [](uint32_t entityId) { StartCutscene(); };
trigger.onExit = [](uint32_t entityId) { ResumeFreeplay(); };
triggers.AddTrigger(trigger);

// AABB trigger
TriggerVolume boxTrigger;
boxTrigger.shape = TriggerShape::AABB;
boxTrigger.center = {200, 0, 100};
boxTrigger.halfExtents = {20, 5, 20};
triggers.AddTrigger(boxTrigger);

// Per-frame
triggers.Update(entityPositions);
```

---

## Input System

**Location:** `SparkEngine/Source/Input/`

### InputManager

**File:** `SparkEngine/Source/Input/InputManager.h`

Comprehensive Windows input handling:

```cpp
InputManager input;
input.Initialize(hwnd);

// Per-frame
input.Update();

// Keyboard
bool pressed = input.IsKeyDown(VK_SPACE);
bool justPressed = input.IsKeyPressed(VK_SPACE);  // Edge detection
bool justReleased = input.IsKeyReleased(VK_SPACE);

// Mouse
bool leftClick = input.IsMouseButtonDown(MouseButton::Left);
POINT mousePos = input.GetMousePosition();
POINT mouseDelta = input.GetMouseDelta();

// Mouse capture (for FPS camera)
input.SetMouseCapture(true);
input.SetMouseSensitivity(2.0f);
input.SetInvertY(false);
input.SetRawInput(true);
```

### Input Bindings

**File:** `SparkEngine/Source/Input/InputBindings.h`

Action-to-key mapping with remapping support:

```cpp
InputBindings bindings;
bindings.Bind("MoveForward", VK_W);
bindings.Bind("MoveBackward", VK_S);
bindings.Bind("Jump", VK_SPACE);
bindings.Bind("Fire", MouseButton::Left);
bindings.Bind("Aim", MouseButton::Right);

bool isMoving = bindings.IsActionActive("MoveForward");
bindings.Rebind("Jump", VK_UP);  // Remap
```

### Gamepad Input

**File:** `SparkEngine/Source/Input/GamepadInput.h`

Controller support via XInput/VK_PAD:

```cpp
GamepadInput gamepad;
float leftStickX = gamepad.GetAxis(GamepadAxis::LeftStickX);
float rightTrigger = gamepad.GetAxis(GamepadAxis::RightTrigger);
bool aButton = gamepad.IsButtonDown(GamepadButton::A);
```

### Console Integration

```cpp
InputMetrics metrics = input.Console_GetMetrics();
// metrics: keyPressCount, mousePressCount, totalMouseDistance
input.Console_SetSensitivity(2.5f);
input.Console_SetDeadzone(0.15f);
```

### Thread Safety

Mutex + atomic counters for concurrent access from input thread and main thread.
