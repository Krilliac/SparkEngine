# Gameplay & Engine Systems Status

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

Comprehensive audit of all 27+ gameplay/engine systems. 19 systems are fully operational (initialized + called each frame or restored with tests). Several systems were deleted as orphaned code and some were restored when found to be legitimate.

---

## Working Systems (19)

All initialized in startup and called in the main loop:

| System | Init Location | Main Loop Call | Key Files |
|--------|--------------|----------------|-----------|
| ECS (EnTT) | CreateGameSystems() in PhaseSystemManager | phaseManager->UpdateAll() | 2 cpp, 20 h |
| Physics (Bullet 3) | SparkEngine.cpp:282,397 | ECS phase manager | 4 cpp, PhysicsSystem.h |
| AI | EngineSetup::RegisterCoreSubsystems() | AIUpdateSystem in phase mgr | 6 cpp (AISystem, NavMesh, BehaviorTree) |
| Animation | Core subsystem registration | AnimationUpdateSystem in phase mgr | 3 cpp, 11 h |
| Audio (XAudio2) | SparkEngine.cpp:468 | AudioUpdateSystem in phase mgr | AudioEngine impl |
| Input | SparkEngine.cpp:623 | g_input->Update() at :510 | 4 cpp in Input/ |
| Camera | Game::Initialize() | UpdateCamera(dt) in Game::Update:579 | SparkEngineCamera.cpp |
| Scripting (AngelScript) | Core subsystem via EngineSetup | Script execution in module lifecycle | 3 cpp with hot-reload |
| Save system | SparkEngine.cpp:308,465 | Via console commands and game code | SaveSystem.cpp (JSON + miniz) |
| UI system | SparkEngine.cpp:423 | m_hudSystem->Update(dt) in Game:606 | UISystem.cpp |
| Modding | SparkEngine.cpp:429 | Registered in EngineContext | ModSystem.cpp |
| Events/EventBus | g_eventBus creation | Publish/subscribe (header-only) | EventSystem.h (495 lines inline) |
| Coroutines | EngineContext::SetCoroutineScheduler() | C++20 coroutines (header-only) | CoroutineScheduler.h |
| 2D graphics/sprites | Phase manager registration | SpriteAnimatorSystem, Sprite2DRenderSystem | Systems2D.h, Physics2D.h |
| Dialogue | SparkEngine.cpp:426 | Branching conversations | DialogueSystem.cpp |
| Weather | SparkEngine.cpp:420 | Day/night cycles, weather effects | WeatherSystem.cpp |
| World origin | World management init | Origin rebasing for large worlds | WorldOriginSystem |
| **Localization** | **RESTORED** — has tests | File-based string tables with regex parsing | LocalizationSystem.cpp (227 lines) |
| **Destruction** | **RESTORED** — has tests | Fracture patterns, physics debris | DestructionSystem.cpp (147 lines) |

---

## Restored Systems (2026-03-16)

### Localization System
- **Files:** `Engine/Localization/LocalizationSystem.cpp` (227 lines), `.h` (197 lines)
- **Test:** `Tests/TestLocalizationSystem.cpp` — tests string table get/set, missing key fallback
- **Functionality:** JSON-based string tables, per-language loading, key lookup with fallback
- **Previously deleted** as "stub-only, never called" — actually a complete implementation with tests

### Destruction System
- **Files:** `Engine/Destruction/DestructionSystem.cpp` (147 lines), `.h` (267 lines)
- **Test:** `Tests/TestDestructionSystem.cpp` — tests fracture patterns, damage application
- **Functionality:** Fracture patterns (wood, metal, concrete), debris physics, damage thresholds
- **Previously deleted** as "never initialized" — complete implementation with built-in presets

---

## Systems Deleted (confirmed orphaned/stub)

These systems were deleted because they were genuinely stub-only with no real implementation:

| System | Files | Size | Why Deleted |
|--------|-------|------|-------------|
| Streaming (seamless areas) | 3 cpp, 4 h | ~1,000+ lines | Never wired in; stub implementation |
| Procedural generation | ProceduralGeneration.cpp | 52KB | Never instantiated or updated |
| Cinematic/Sequencer | Sequencer.cpp | 29KB | Never initialized |
| Replay system | ReplaySystem.h | Large | Never initialized |
| Achievement system | AchievementSystem.h | Medium | Never initialized |
| Content Delivery | ContentDelivery.cpp/h | 337 lines | Stub-only CDN framework, no HTTP |
| Visual Scripting (2 copies) | 7,343 lines total | 7.3KB | Two independent implementations, neither wired in |

---

## Missing Systems (3)

| System | Status | Notes |
|--------|--------|-------|
| Inventory (engine-level) | NO CODE | Quest system references inventory but no InventorySystem exists |
| Quest (engine-level) | GAME-ONLY | SparkGame/Source/Game/QuestSystem.h exists but no reusable Spark::QuestSystem |
| Weapon (engine-level) | GAME-ONLY | WeaponManager.h partial; weapon logic scattered in SparkGame only |

---

## Notes

- Networking is fully implemented but opt-in via ENABLE_NETWORKING; acceptable as-is
- VR system is registered via core subsystems — actually working
- Loading screens are partially wired
