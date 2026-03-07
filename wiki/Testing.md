# Testing

SparkEngine includes 35 unit tests using a lightweight internal test framework with CTest integration.

**Source:** `Tests/TestFramework.h`, `Tests/`

## Test Framework

The engine uses its own lightweight test framework (no external test library dependencies):

### Test Macros

```cpp
#include "TestFramework.h"

TEST(MyTestName) {
    EXPECT_EQ(value, expected);        // Equality check
    EXPECT_TRUE(condition);            // Boolean check
    EXPECT_FALSE(condition);           // Negative boolean check
    EXPECT_NEAR(a, b, tolerance);      // Floating-point comparison
    EXPECT_NE(value, unexpected);      // Not-equal check
}
```

## Running Tests

### Build with Tests Enabled

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
```

### Run All Tests

```bash
ctest --test-dir build --output-on-failure
```

### Run Specific Tests

```bash
ctest --test-dir build -R "TestPhysics"           # Run tests matching pattern
ctest --test-dir build -R "TestECS|TestAnimation"  # Run multiple patterns
```

## Test Coverage

The 35 unit tests cover all major subsystems:

| Category | Tests |
|----------|-------|
| **Math** | Math utilities, vector operations |
| **Core** | Object pool, ring buffer |
| **ECS** | Entity creation, component operations, views |
| **Physics** | Body creation, raycasting, collision |
| **AI** | Behavior trees, NavMesh pathfinding |
| **Animation** | State machines, blending, IK |
| **Audio** | Audio source management |
| **Input** | Input state tracking |
| **Scripting** | Script compilation, lifecycle |
| **Save System** | Serialization, compression |
| **Events** | Event bus subscribe/publish |
| **Weather** | Weather transitions |
| **Inventory** | Item management |
| **Quests** | Quest tracking |
| **Day/Night** | Time progression |
| **Lighting** | Light calculations |
| **Performance** | Profiler, frame stats |
| **Fog** | Fog calculations |
| **Screen-Space** | SSAO, SSR parameters |
| **Post-Processing** | Effect pipeline |
| **Sequencer** | Cinematic timeline |
| **Mesh LOD** | LOD switching |
| **Console** | Command parsing |
| **Coroutines** | Coroutine scheduling |
| **Tweening** | Animation tweens |
| **Temporal Effects** | TAA settings |
| **Noise** | Procedural noise generation |
| **Game Modes** | Game mode management |

## Adding a New Test

1. Create a test file in `Tests/`:

```cpp
// Tests/TestMyFeature.cpp
#include "TestFramework.h"
#include "MyFeature.h"

TEST(MyFeature_BasicTest) {
    MyFeature feature;
    feature.Initialize();
    EXPECT_TRUE(feature.IsReady());
}

TEST(MyFeature_EdgeCase) {
    MyFeature feature;
    EXPECT_EQ(feature.Compute(0), 0);
    EXPECT_NEAR(feature.Compute(1.0f), 1.0f, 0.001f);
}
```

2. The test is automatically discovered by CMake (tests are globbed in `Tests/CMakeLists.txt`). See [[Build System and CMake Modules]] for build configuration.

3. Build and run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

## CI Integration

Tests run automatically on every push via GitHub Actions:
- Windows (VS 2022) — Debug and Release
- Linux (GCC, Clang) — Debug and Release
- Sanitizer builds (ASan, TSan) for memory and thread safety

---

## See Also

- [[Build System and CMake Modules]] — BUILD_TESTS flag
- [[Getting Started]] — Building the project
- [[Contributing]] — Contribution workflow and adding tests
