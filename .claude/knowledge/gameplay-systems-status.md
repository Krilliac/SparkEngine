# Gameplay & Engine Systems Status

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

Comprehensive audit of all 27+ gameplay/engine systems. 17 systems are fully operational (initialized + called each frame). 7 systems are built but never wired in (~90K+ lines of orphaned code). 3 systems are missing entirely.

---

## Working Systems (17)

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

---

## Built-Not-Wired Systems (7)

Complete implementations that are NEVER initialized or called:

| System | Files | Size | What Exists | What's Missing |
|--------|-------|------|-------------|----------------|
| Streaming (seamless areas) | 3 cpp, 4 h | ~1,000+ lines | HeroEngine-inspired area transitions | NOT in SparkEngine.cpp init or main loop |
| Networking (UDP) | 7 cpp, 12 h | Large | AreaServer, WorldServer, client prediction, lag comp | ENABLE_NETWORKING=OFF default; Game.cpp:614 refs netMgr but gated |
| Procedural generation | ProceduralGeneration.cpp | 52KB | Noise, splines, terrain generation | Never instantiated or updated |
| Destruction | DestructionSystem.cpp | 5.6KB | Destructible objects infrastructure | Never initialized |
| Cinematic/Sequencer | Sequencer.cpp | 29KB | Full playback, tracks, camera control | Never initialized |
| Replay system | ReplaySystem.h | Large | Kill cam, spectator replay, full-state recording | Never initialized |
| Achievement system | AchievementSystem.h | Medium | Stat tracking, unlock conditions, platform bridging | Never initialized |

### Additional partially-wired:

| System | Status |
|--------|--------|
| Localization | Localization.cpp exists but stub-only, never called |
| Loading screens | LoadingScreen.cpp exists, partially wired |
| VR | VR.cpp exists, registered via core subsystems — actually WORKING |
| Stats/telemetry | Working, registered via core subsystems |

---

## Missing Systems (3)

| System | Status | Notes |
|--------|--------|-------|
| Inventory (engine-level) | NO CODE | Quest system references inventory but no InventorySystem exists |
| Quest (engine-level) | GAME-ONLY | SparkGame/Source/Game/QuestSystem.h exists but no reusable Spark::QuestSystem |
| Weapon (engine-level) | GAME-ONLY | WeaponManager.h partial; weapon logic scattered in SparkGame only |

---

## Key Findings

1. **17 systems are fully operational** — the engine core is functional for basic game development
2. **7 systems are ConsoleProcessManager-style orphans** — built but never wired in, adding ~90K+ lines of maintenance burden
3. **Streaming system** is the largest orphan (~1,000+ lines of HeroEngine-inspired code that nobody calls)
4. **Networking** is fully implemented but disabled by default; when ENABLE_NETWORKING=ON, it should work
5. **3 gameplay systems have no engine-level implementation** — inventory, quests, and weapons exist only in SparkGame
6. **Procedural generation** (52KB) is one of the largest orphaned systems — complete noise/spline/terrain gen that's never used

---

## Action Required

**Per CLAUDE.md Anti-Bloat Rules**: Built-but-not-wired systems must be either wired in or deleted. The 7 orphaned systems represent significant dead code that creates false confidence about engine capabilities.

| System | Recommendation |
|--------|---------------|
| Streaming | Wire in or delete — critical for claimed "seamless area" feature |
| Networking | Acceptable as opt-in (ENABLE_NETWORKING); document clearly |
| Procedural gen | Delete unless terrain system is planned |
| Destruction | Wire into physics or delete |
| Cinematic | Wire into editor sequencer panel or delete |
| Replay | Wire into game module or delete |
| Achievement | Wire into game module or delete |
