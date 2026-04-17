# Memory Integrity System

SparkEngine's Memory Integrity System detects runtime code tampering and validates critical execution paths. Inspired by Warden-style anti-cheat code scanning, it provides two layers of protection: periodic code page hashing to detect binary patches, and branch guards that prove critical security checks actually execute.

**Source:** `SparkEngine/Source/Engine/Security/MemoryIntegrity.h`, `MemoryIntegrity.cpp`

## Architecture Overview

The system operates on two complementary layers:

```
                    LAYER 1: CODE REGION SCANNING
                    ==============================

  Engine Startup          MemoryIntegritySystem          Violation
  +---------------+       +---------------------+        +------------------+
  | Auto-discover |------>| Snapshot FNV-1a      |        | EventBus publish |
  | code pages    |       | hashes of all        |        | ViolationCallback|
  | (VirtualQuery |       | registered regions   |        | SPARK_LOG_WARN   |
  |  /proc/maps)  |       +---------------------+        +------------------+
  +---------------+               |                              ^
                            Every N seconds                      |
                                  |                              |
                                  v                              |
                          +---------------------+                |
                          | Re-hash batch of    |--- mismatch -->|
                          | regions, compare    |
                          | against snapshots   |
                          +---------------------+


                    LAYER 2: BRANCH GUARDS
                    ======================

  Critical Code Path      MemoryIntegritySystem          Violation
  +------------------+    +---------------------+        +------------------+
  | SPARK_BRANCH_    |    | RecordBranchExec()  |        | BranchBypassed   |
  | GUARD_BEGIN(id)  |--->| Sets executionCount  |        | if count == 0    |
  |                  |    +---------------------+        +------------------+
  | if (check) {...} |                                          ^
  |                  |    +---------------------+               |
  | SPARK_BRANCH_    |    | VerifyBranchExec()  |--- no exec -->|
  | GUARD_END(id)    |--->| Checks count > 0    |
  +------------------+    | Resets for next      |
                          | cycle                |
                          +---------------------+
```

## Source Files

| File | Responsibility |
|------|---------------|
| `Engine/Security/MemoryIntegrity.h` | System class, types, macros |
| `Engine/Security/MemoryIntegrity.cpp` | Implementation, platform code scanning, console commands |
| `Tests/TestMemoryIntegrity.cpp` | 16 unit tests |

## Key Types

### Violation Types

| Type | Meaning |
|------|---------|
| `CodeModified` | A registered code region's FNV-1a hash no longer matches its snapshot |
| `BranchBypassed` | A branch guard was never executed between verify calls (NOP'd) |
| `BranchWrongPath` | A branch guard recorded an unexpected path value |
| `FunctionModified` | A registered function body was modified |

### Severity Levels

| Severity | Meaning |
|----------|---------|
| `Warning` | Suspicious but may be benign (e.g., first-time branch miss) |
| `Critical` | Strong evidence of tampering (e.g., 3+ consecutive bypasses, code region modified) |

Severity escalates automatically: a branch bypassed once is `Warning`, but 3+ consecutive bypasses escalate to `Critical`.

## Quick Start

### Protecting a critical branch

```cpp
#include "Engine/Security/MemoryIntegrity.h"

void ValidateDamage(Entity target, float damage)
{
    SPARK_BRANCH_GUARD_BEGIN("damage_validation")
    if (damage > 0.0f && damage < MAX_DAMAGE)
    {
        ApplyDamage(target, damage);
    }
    SPARK_BRANCH_GUARD_END("damage_validation")
}
```

If an attacker NOP's the branch instruction, the `RecordBranchExecution` call inside `SPARK_BRANCH_GUARD_BEGIN` never fires, and `VerifyBranchExecuted` in `SPARK_BRANCH_GUARD_END` flags a `BranchBypassed` violation.

### Simple checkpoint pattern

```cpp
void ValidateMovement(Entity player, XMFLOAT3 newPos)
{
    SPARK_INTEGRITY_CHECKPOINT("movement_check")
    float dist = Distance(GetPosition(player), newPos);
    if (dist > MAX_MOVE_PER_FRAME)
    {
        RejectMovement(player);
        return;
    }
    SPARK_VERIFY_CHECKPOINT("movement_check")
    ApplyMovement(player, newPos);
}
```

### Registering a code region manually

```cpp
auto& mis = Spark::Security::MemoryIntegritySystem::GetInstance();
mis.RegisterCodeRegion("my_critical_func", reinterpret_cast<const void*>(&MyCriticalFunc), 256);
mis.RegisterFunction("another_func", reinterpret_cast<const void*>(&AnotherFunc), 128);
```

### Responding to violations

```cpp
mis.SetViolationCallback([](const Spark::Security::Violation& v) {
    if (v.severity == Spark::Security::ViolationSeverity::Critical)
    {
        // Game-specific response: kick player, flag account, log telemetry
        KickPlayer("Memory integrity violation detected");
    }
});
```

## Macro Reference

| Macro | Purpose |
|-------|---------|
| `SPARK_BRANCH_GUARD_BEGIN(name)` | Begin a protected branch scope; registers guard and records true-path |
| `SPARK_BRANCH_GUARD_ELSE(name)` | Record the else-path of a protected branch |
| `SPARK_BRANCH_GUARD_END(name)` | End protected scope and verify the branch executed |
| `SPARK_INTEGRITY_CHECKPOINT(name)` | Record that a critical code path executed (one-shot) |
| `SPARK_VERIFY_CHECKPOINT(name)` | Verify that the matching checkpoint actually ran |

All macros use compile-time FNV-1a hashing of the name string, so there is zero runtime string comparison cost.

## Where Guards Are Wired In

### Protected systems (guards active)

| System | File | What's protected | Bypass impact |
|--------|------|-----------------|---------------|
| **Packet Validator** | `PacketValidator.cpp` | Payload size, auth, direction checks | Buffer overflow, privilege escalation |
| **Packet Gateway** | `NetworkConnection.cpp` | Validation pipeline entry point | Malicious packets reach game logic |
| **RCON Gate** | `DedicatedServer.cpp` | Chat-to-RCON command gate | Any player runs admin commands |
| **Console RBAC** | `SparkConsole.cpp` | Permission level check | Players run developer commands |
| **Script Sandbox** | `ScriptSandbox.cpp` | Instruction/timeout/memory limits | DoS, sandbox escape |
| **Ability Cooldowns** | `AbilitySystem.cpp` | Cooldown check, damage validation, death/kill check, health cap | Ability spam, god mode, infinite health |
| **Inventory** | `InventorySystem.cpp` | Add/remove validation, capacity check | Item duplication, unlimited inventory |
| **FPS Player** | `Player.cpp` | Death check on health <= 0 | God mode |
| **FPS Console** | `PlayerConsole.cpp` | Speed and jump height validation | Speed hacking, flight |
| **MMO Trading** | `MMOTradingSystem.cpp` | Trade state validation (must be Locked) | Dupe trades, currency exploits |
| **ARPG Skills** | `ARPGSkillSystem.cpp` | Cooldown check and cooldown application | Instant ability spam |

### Excluded systems (NOT guarded)

| System | File | Reason |
|--------|------|--------|
| **MemoryIntegrity itself** | `MemoryIntegrity.cpp` | Self-referential circular dependency; code pages are auto-scanned instead |
| **Script Hot-Reload** | `ScriptHotReload.cpp` | Hot-reload intentionally modifies code at runtime; would cause false positives |
| **RHI Validation Layer** | `RHIValidationLayer.cpp` | Debug-only, not security-critical; attackers gain nothing from bypassing |
| **Physics System** | `PhysicsSystem.cpp` | Per-frame hot path; overhead unacceptable in simulation tick. Validate at network layer instead |
| **Graphics Engine** | `GraphicsEngine.cpp` | Per-frame render loop; graphics cheats are better detected server-side |
| **Editor** | `SparkEditor/` (all) | Development tool, not shipping client; guards would interfere with debugging |

## Platform Support

### Code Region Auto-Discovery

| Platform | Method | Details |
|----------|--------|---------|
| **Windows** | `VirtualQuery()` | Walks module memory pages, filters for `PAGE_EXECUTE_READ/READWRITE` |
| **Linux** | `/proc/self/maps` | Parses executable regions (`r-xp`), caps at 4 MB per region |
| **macOS** | Manual only | No auto-discovery; use `RegisterCodeRegion()` explicitly |

### Hashing

All checksums use `Spark::FNV1a64(const void* data, size_t size)` from `Utils/Hash.h` — a `constexpr`-capable, dependency-free hash with excellent distribution for code bytes.

## Configuration

```cpp
Spark::Security::IntegrityConfig config;
config.scanIntervalSec = 5.0f;     // Seconds between periodic scans (default: 5)
config.batchSize = 8;              // Regions scanned per batch (default: 8)
config.autoDiscoverRegions = true; // Use platform APIs for code pages (default: true)
config.enabled = true;             // Master enable switch (default: true)

auto& mis = Spark::Security::MemoryIntegritySystem::GetInstance();
mis.Configure(config);
```

## Console Commands

All commands require `Developer` permission level.

| Command | Description |
|---------|-------------|
| `memory.integrity.status` | Show regions monitored, branch guards, violation count, scan timing |
| `memory.integrity.scan` | Force immediate full scan of all registered regions |
| `memory.integrity.violations` | List up to 20 most recent violations with type, severity, and hashes |

## Violation Response Model

Violations are reported through three channels simultaneously:

1. **Logging** — `SPARK_LOG_WARN` with violation details
2. **EventBus** — `MemoryViolationEvent` published to `EventBus::Global()`
3. **Callback** — User-registered `ViolationCallback` function

The engine does **not** automatically disconnect or ban players. Game modules register a callback and decide the response based on their own policies (severity thresholds, violation counts, trusted/untrusted contexts).

## How It Detects Common Attacks

### NOP Slide (patching JMP/JNZ to 0x90)

When an attacker NOP's a conditional branch:
1. The `SPARK_BRANCH_GUARD_BEGIN` macro calls `RecordBranchExecution()` **before** the branch instruction
2. If the branch instruction itself is NOP'd, execution falls through without taking either path
3. `SPARK_BRANCH_GUARD_END` calls `VerifyBranchExecuted()` which checks that `RecordBranchExecution` was called
4. If the attacker NOP'd the BEGIN macro too, the execution count stays at 0 → `BranchBypassed`

### Code Page Patching

When an attacker modifies bytes in executable memory:
1. At startup, the system snapshots FNV-1a hashes of all executable code pages
2. Every `scanIntervalSec` seconds, it re-hashes a batch of regions
3. If any hash doesn't match → `CodeModified` violation with `Critical` severity

### Function Body Modification

When an attacker modifies a specific function:
1. Register the function with `RegisterFunction("name", &func, estimatedSize)`
2. The system hashes the function bytes at registration time
3. Periodic scanning detects any modification

## Testing

16 unit tests in `Tests/TestMemoryIntegrity.cpp`:

| Test | What it verifies |
|------|-----------------|
| `RegisterAndScanClean` | Clean buffer hashes match |
| `DetectModifiedRegion` | Tampered bytes detected as `CodeModified` |
| `RegisterFunction` | Function registration works |
| `NullRegionIgnored` | Null/zero-size regions rejected |
| `BatchScanning` | Batch cursor advances correctly |
| `BranchGuardNormal` | Normal execution produces no violations |
| `BranchGuardBypassed` | Skipped execution detected as `BranchBypassed` |
| `BranchGuardRepeatedBypassEscalates` | 3+ bypasses escalate to `Critical` |
| `BranchGuardUnknownId` | Unknown guard ID = `Critical` violation |
| `BranchGuardResetsPerCycle` | Execution counter resets between verify cycles |
| `ViolationCallback` | Callback fires on violation |
| `MetricsTracking` | Region/guard/scan counts accurate |
| `CheckpointMacros` | Checkpoint + verify pattern works |
| `AutoRegisterOnRecord` | Auto-registration on first RecordBranchExecution |
| `StatusString` | Status string contains expected fields |
| `UpdateTriggersPeriodicScan` | Timer-driven scanning fires at interval |

Run tests:
```bash
cmake --build build --config Release
cd build && ctest --output-on-failure
# Or run directly:
./build/linux-gcc-release/bin/SparkTests 2>&1 | grep MemoryIntegrity
```

## Engine Integration

The system is wired into the standard engine lifecycle:

- **Initialize**: `InitDebugSystems()` in `GameplaySystemLifecycle.cpp`
- **Update**: `UpdateDebugSystems(dt)` via `SPARK_GUARDED_UPDATE("MemoryIntegrity", "Security", ...)`
- **Shutdown**: `ShutdownDebugSystems()`

Active in **all builds** (Debug and Release). Protection must be present in shipping builds to be effective.

## Best Practices

1. **Guard security boundaries, not every branch.** Focus on permission checks, validation gates, cooldown enforcement, and economy operations.
2. **Don't guard hot paths.** Physics ticks, render loops, and per-vertex operations should not have branch guards.
3. **Use checkpoints for linear flows.** `SPARK_INTEGRITY_CHECKPOINT` / `SPARK_VERIFY_CHECKPOINT` for code that must execute in sequence.
4. **Use branch guards for if/else flows.** `SPARK_BRANCH_GUARD_BEGIN/END` around security-critical conditionals.
5. **Register game-specific callbacks.** The engine reports violations; your game module decides the response.
6. **Test with tampering.** Write tests that simulate NOP'd branches (skip `RecordBranchExecution`, then call `VerifyBranchExecuted`).

## See Also

- [Networking](Networking.md) — Network security, packet encryption, connection tokens
- [Scripting with AngelScript](Scripting-with-AngelScript.md) — Script sandbox enforcement
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) — Ability, inventory, and quest systems
- [Testing](../advanced/Testing.md) — Test framework and running tests
