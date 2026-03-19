# Globals, Singletons, and Architectural Audit

**Last updated:** 2026-03-18
**Type:** Observation
**Status:** Active
**Severity:** Medium

## Singleton Census — 26 Total

### Wired In (20 — functioning correctly)
1. SimpleConsole — Initialize()/Update() called in all startup paths
2. Profiler — Used in multiple systems
3. CoroutineScheduler — Registered with EngineContext
4. EngineSettings — Load() called at startup
5. NetworkManager — Initialized when ENABLE_NETWORKING=ON
6. MusicManager — Initialize()/Update()/Shutdown() in Init/Update/ShutdownGameplaySystems() (wired 2026-03-18)
7. AudioMixer (AudioBusMixer) — Update() called from MusicManager
8. AngelScriptEngine — Initialize() called
9. SaveSystem — Initialize() called at startup
10. ConsoleProcessManager — Initialize() + ProcessCommands() in all startup/loop paths (wired 2026-03-17)
11. FileLogger — Initialize("Logs") in InitDebugSystems() (wired 2026-03-17)
12. ChromeTracing — Start()/SaveToFile()/Stop() in Init/ShutdownDebugSystems() (wired 2026-03-17)
13. MemoryDebugger — SetEnabled()/PrintLeakReport() in Init/ShutdownDebugSystems() (wired 2026-03-17)
14. TweenManager — Update()/KillAll() in Update/ShutdownDebugSystems() (wired 2026-03-17)
15. DebugDrawManager — SetEnabled()/Flush()/Clear() in Init/Update/ShutdownDebugSystems() (wired 2026-03-17)
16. DebugOverlay — SetEnabled()/Update() in Init/UpdateDebugSystems() (wired 2026-03-17)
17. DecalSystem — Initialize()/Update()/Shutdown() in Init/Update/ShutdownDebugSystems() (wired 2026-03-17)
18. FrameInspector — OnFrameEnd() in UpdateDebugSystems() (wired 2026-03-17)
19. WeaponRegistry — RegisterDefaults() in InitDebugSystems() (wired 2026-03-17)
20. ConditionSystem/AbilitySystem/InstanceManager/MovementSystem — All wired in Init/Update/ShutdownGameplaySystems() (TrinityCore, 2026-03-18)
21. CoroutineScheduler — Update(dt) in UpdateGameplaySystems() (wired 2026-03-18)

22. WeaponSystem — Update(dt) via static local in UpdateGameplaySystems(); iterates WeaponInventoryComponent via EnTT view (wired 2026-03-18)

### Passive Caches (no lifecycle methods — working as designed)
1. **AnimationManager** — Asset cache singleton; GetClip()/GetSkeleton() called by AnimationSystem.cpp at runtime
2. **NavMeshManager** — Passive registry. AISystem queries it lazily via CreateQuery("default")
3. **NavMeshObstacleManager** — Passive manager for dynamic NavMesh carving. Wire SetNavMesh() when dynamic obstacles are needed
4. **LODManager** — Passive cache for mesh LOD chains. SelectLOD() queried at render time when chains are registered
5. **DXRManager** — Gated behind ENABLE_DXR=OFF. Not orphaned, just disabled by feature flag

### Deleted
- **PlatformInputManager** — Deleted 2026-03-18 (5 files, ~2,100 lines, dead duplicate of InputManager)
- VisualScriptSystem — deleted (duplicate, 2026-03-17)
- SequencerManager — class doesn't exist in codebase (was documentation-only reference)

### Deleted (confirmed removed in prior sessions)
- VisualScriptSystem — deleted (duplicate, 2026-03-17)
- SequencerManager — class doesn't exist in codebase (was documentation-only reference)

## Global Variables

| Variable | File | Status |
|----------|------|--------|
| `g_graphics`, `g_input`, `g_timer`, `g_eventBus`, `g_moduleManager`, `g_audioEngine` | SparkEngine.cpp | Active engine globals |
| `g_physicsOwned` | SparkEngine.cpp | Guarded by SPARK_BULLET_PHYSICS_AVAILABLE |
| `g_moduleHotReload` | SparkEngine.cpp | Module hot-reload watcher |
| `g_MainSwapChain`, `g_D3DDevice`, `g_D3DContext` | D3DUtils.cpp | Legacy; wrapped with accessors |
| `g_hInst`, `g_szTitle`, `g_szClass` | SparkEngine.cpp | Windows platform globals |
| `g_headlessMode` | EngineContext.cpp | Guarded by SPARK_HEADLESS_SUPPORT |

D3D11 globals are set once during initialization. No synchronization, but single-threaded access pattern is enforced by convention.

## God Object: GraphicsEngine

**74 member variables** including:
- 7 smart pointers
- 15 COM pointers
- 7 D3D state objects
- 8 timing objects
- 8+ settings/flags
- 7+ subsystem references

Needs decomposition into modules (lighting, materials, textures, etc.). File was split from 4,949 to 5 files in 2026-03-18 session, but the class itself still has too many members.

## Thread Safety Assessment

| System | Mechanism | Status |
|--------|-----------|--------|
| SimpleConsole | 3 mutexes (log, command, history) | Safe |
| ConsoleProcessManager | 2 mutexes (message, command) | Safe |
| GraphicsEngine | `std::atomic<bool> m_frameInProgress` | Safe |
| Profiler | `mutable std::mutex m_metricsMutex` | Safe |
| PhysicsSystem | None (main-thread-only by contract) | Intentional |
| InputManager | None (main-thread-only) | Intentional |
| D3D11 globals | None (set once at init) | Fragile |

Thread safety is intentionally asymmetric per CLAUDE.md spec.

## Notes

- No circular header dependencies detected
- Magic numbers are minimal; most wrapped in constexpr
- g_graphics global is still in active use despite EngineContext existing (56 command lambdas reference it via GetGfx() helper)
- 10 TrinityCore-inspired singletons all properly wired into Init/Update/Shutdown paths
