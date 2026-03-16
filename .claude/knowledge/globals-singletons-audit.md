# Globals, Singletons, and Architectural Audit

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** Medium

## Singleton Census — 26 Total

### Wired In (14 — functioning correctly)
1. SimpleConsole — Initialize()/Update() called
2. Profiler — Used in multiple systems
3. CoroutineScheduler — Registered with EngineContext
4. EngineSettings — Load() called at startup
5. NetworkManager — Initialized when ENABLE_NETWORKING=ON
6. MusicManager — Initialize() called
7. AudioMixer (MusicManager's) — Update() called from MusicManager
8. AngelScriptEngine — Initialize() called
9. SaveSystem — Initialize() called at startup
10-14. Various others with confirmed call sites

### Orphaned (12 — never initialized/called)
1. ConsoleProcessManager — Initialize()/ProcessCommands() never called
2. VisualScriptSystem — Initialize() never called
3. DecalSystem — Initialize()/Update() never called
4. FileLogger — Initialize() never called
5. DXRManager — Initialize() never called
6. PlatformInputManager — InputManager used instead
7. AnimationManager — Update() never called
8. NavMeshManager — Never instantiated
9. NavMeshObstacleManager — GetInstance() never called
10. SequencerManager — GetInstance()/Update() never called
11. WeaponManager — Never initialized
12. MeshLOD/LODManager — GetInstance() never called

### Dead Code Singletons (3 — should be deleted)
1. ChromeTracing — 0 usages
2. MemoryDebugger — 0 usages
3. TweenManager — 0 usages

## Global Variables

| Variable | File | Status |
|----------|------|--------|
| `g_MainSwapChain`, `g_D3DDevice`, `g_D3DContext` | D3DUtils.cpp | Legacy; wrapped with accessors |
| `g_hInst`, `g_szTitle`, `g_szClass` | SparkEngine.cpp | Windows platform globals |
| `g_headlessMode` | EngineContext.cpp | Guarded by SPARK_HEADLESS_SUPPORT |
| `g_engineContext` | EngineContext.cpp | Service locator singleton |

D3D11 globals are set once during initialization. No synchronization, but single-threaded access pattern is enforced by convention.

## God Object: GraphicsEngine

**74 member variables** including:
- 7 smart pointers
- 15 COM pointers
- 7 D3D state objects
- 8 timing objects
- 8+ settings/flags
- 7+ subsystem references

Needs decomposition into modules (lighting, materials, textures, etc.).

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
