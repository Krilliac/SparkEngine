# Memory Integrity System

> **Audience:** Programmers
>
> **Thread Context:** Singleton initialized in `InitDebugSystems()` and ticked via `SPARK_GUARDED_UPDATE` inside `UpdateDebugSystems(dt)` on the main thread. Code-region scanning re-verifies executable pages in periodic batches. Violations are dispatched to Log + EventBus + a user callback.
>
> **Platform/Backend Scope:** Active in all builds (Debug and Release). Code-region discovery uses `VirtualQuery` on Windows and `/proc/self/maps` on Linux.

## Overview

The Memory Integrity System provides runtime anti-tamper protection. It detects code tampering (NOP'd branches, modified function bodies) and proves that security-critical execution paths actually run. It lives in `SparkEngine/Source/Engine/Security/MemoryIntegrity.h` / `.cpp`.

## Architecture

Two layers:

1. **Code region scanning** — FNV-1a hashes of executable memory pages with periodic batch re-verification. Regions are auto-discovered via `VirtualQuery` (Windows) or `/proc/self/maps` (Linux).
2. **Branch guards** — macros (`SPARK_BRANCH_GUARD_BEGIN`/`END`, `SPARK_INTEGRITY_CHECKPOINT`/`SPARK_VERIFY_CHECKPOINT`) that prove a security-critical branch actually executed.

## Where guards are wired in

Verified present (2026-06-08) in all of the following:

- `PacketValidator.cpp` — payload size, auth, direction checks
- `NetworkConnection.cpp` — packet validation gateway
- `DedicatedServer.cpp` — RCON command gate
- `SparkConsole.cpp` — RBAC permission check
- `ScriptSandbox.cpp` — instruction / timeout / memory limits
- `AbilitySystem.cpp` — cooldowns, damage validation, death check, health cap
- `InventorySystem.cpp` — add/remove validation, capacity check
- `Player.cpp` (FPS module) — death check
- `PlayerConsole.cpp` (FPS module) — speed / jump validation
- `MMOTradingSystem.cpp` — trade state validation
- `ARPGSkillSystem.cpp` — cooldown check and application

## Where guards are deliberately NOT included

| Location | Reason |
|---|---|
| `MemoryIntegrity.cpp` | Circular self-reference; its code pages are auto-scanned anyway |
| `ScriptHotReload.cpp` | Intentionally modifies code at runtime |
| `RHIValidationLayer.cpp` | Debug-only, not security-critical |
| `PhysicsSystem.cpp` | Per-frame hot path — overhead unacceptable |
| `GraphicsEngine.cpp` | Per-frame render loop hot path |
| `SparkEditor/` | Development tool, not the shipping client |

## Key patterns

- Singleton via `GetInstance()`, initialized in `InitDebugSystems()`.
- Ticked via `SPARK_GUARDED_UPDATE` inside `UpdateDebugSystems(dt)`.
- Violation response: Log + EventBus event + user callback (the game decides what to do).
- Console commands: `memory.integrity.status` / `scan` / `violations` (Developer permission).
- Test coverage: `Tests/TestMemoryIntegrity.cpp` (16 tests, confirmed 2026-06-08).
- Active in both Debug and Release builds.

## Macro usage

```cpp
// Conditional branch protection
SPARK_BRANCH_GUARD_BEGIN("name")
if (condition) { ... }
SPARK_BRANCH_GUARD_END("name")

// Linear-flow checkpoint
SPARK_INTEGRITY_CHECKPOINT("name")
// critical code
SPARK_VERIFY_CHECKPOINT("name")
```

## Source & Freshness

- Original entry: `.claude/knowledge/memory-integrity-system.md` (created 2026-04-05).
- Verified against codebase 2026-06-08.

Status changes / verifications found during freshening:

- `MemoryIntegrity.h` and `.cpp` both present in `Engine/Security/`.
- All 11 documented guard call sites confirmed via grep for `SPARK_BRANCH_GUARD_BEGIN`/`SPARK_INTEGRITY_CHECKPOINT` (engine + FPS/MMO/ARPG modules).
- `Tests/TestMemoryIntegrity.cpp` confirmed at 16 tests — unchanged.
- No status changes; entry is current.

## Related Pages

- [Memory Integrity](../subsystems/Memory-Integrity.md)
- [Memory Safety Evaluation](Memory-Safety-Evaluation.md)
- [Memory Safety](Memory-Safety.md)
- [Memory Management Patterns](Memory-Management-Patterns.md)
- [Stub & Abandoned Features](Stub-and-Abandoned-Features.md)
