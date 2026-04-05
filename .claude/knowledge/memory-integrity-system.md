# Memory Integrity System

**Type:** Observation / Pattern  
**Status:** Active  
**Created:** 2026-04-05  

## What

Runtime memory integrity and branch protection system in `SparkEngine/Source/Engine/Security/MemoryIntegrity.h/.cpp`. Detects code tampering (NOP'd branches, modified function bodies) and validates critical execution paths.

## Architecture

Two layers:
1. **Code region scanning** — FNV-1a hashes of executable memory pages, periodic batch re-verification. Auto-discovers via VirtualQuery (Windows) or /proc/self/maps (Linux).
2. **Branch guards** — Macros (`SPARK_BRANCH_GUARD_BEGIN/END`, `SPARK_INTEGRITY_CHECKPOINT/VERIFY`) that prove security-critical branches actually execute.

## Where guards are wired in

- `PacketValidator.cpp` — payload size, auth, direction checks
- `NetworkConnection.cpp` — packet validation gateway
- `DedicatedServer.cpp` — RCON command gate
- `SparkConsole.cpp` — RBAC permission check
- `ScriptSandbox.cpp` — instruction/timeout/memory limits
- `AbilitySystem.cpp` — cooldowns, damage validation, death check, health cap
- `InventorySystem.cpp` — add/remove validation, capacity check
- `Player.cpp` (FPS) — death check
- `PlayerConsole.cpp` (FPS) — speed/jump validation
- `MMOTradingSystem.cpp` — trade state validation
- `ARPGSkillSystem.cpp` — cooldown check and application

## Where guards are NOT included (and why)

- `MemoryIntegrity.cpp` — circular self-reference; code pages auto-scanned
- `ScriptHotReload.cpp` — intentionally modifies code at runtime
- `RHIValidationLayer.cpp` — debug-only, not security-critical
- `PhysicsSystem.cpp` — per-frame hot path, unacceptable overhead
- `GraphicsEngine.cpp` — per-frame render loop hot path
- `SparkEditor/` — development tool, not shipping client

## Key patterns

- Singleton with `GetInstance()`, initialized in `InitDebugSystems()`
- Updated via `SPARK_GUARDED_UPDATE` in `UpdateDebugSystems(dt)`
- Violations: Log + EventBus + user callback (game decides response)
- Console commands: `memory.integrity.status/scan/violations` (Developer permission)
- 16 tests in `Tests/TestMemoryIntegrity.cpp`
- Active in all builds (Debug + Release)

## Macro usage

```cpp
// Conditional branch protection
SPARK_BRANCH_GUARD_BEGIN("name")
if (condition) { ... }
SPARK_BRANCH_GUARD_END("name")

// Linear flow checkpoint
SPARK_INTEGRITY_CHECKPOINT("name")
// critical code
SPARK_VERIFY_CHECKPOINT("name")
```
