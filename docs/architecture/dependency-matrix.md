# SparkEngine Dependency Matrix and Cross-Utilization Guardrails

This document defines enforced architecture boundaries between SparkEngine layers and subsystems.
It is paired with `tools/check-cross-utilization.sh`, which validates these rules in CI/local checks.

## Layer dependency matrix

| Layer | Allowed dependencies | Disallowed dependencies |
|---|---|---|
| **SDK ABI** (`SparkSDK/Include`) | STL + stable forward declarations | Concrete engine private headers (`SparkEngine/Source/**`) |
| **Core Runtime** (`SparkEngine/Source/Core`) | SDK ABI, platform adapters, utility services | Game module implementations |
| **Subsystems** (`SparkEngine/Source/Engine/*`, `SparkEngine/Source/Graphics/*`, etc.) | Core services + own-domain contracts | Direct lateral concrete includes between unrelated domain internals |
| **ECS Systems** (`SparkEngine/Source/Engine/ECS/Systems`) | Component data, context services, events | Direct sibling system orchestration via manager lookups in hot path |
| **Game Modules** (`GameModules/*`) | `SparkSDK/Include` ABI (`IModule`, `IEngineContext`) | Engine private implementation headers (`SparkEngine/Source/**`) |

## Enforced guardrails

### 1) Game module include boundary

**Rule:** Files under `GameModules/` must not include private engine source headers.

**Reason:** Protect module ABI stability and keep module code buildable against SDK-only contracts.

**Validated by:** regex check for `#include "SparkEngine/Source/..."`.

---

### 2) Deprecated global usage boundary

**Rule:** Deprecated globals (`g_graphics`, `g_input`, etc.) are allowed only in lifecycle bridge files during migration.

**Current allowlist:**
- `SparkEngine/Source/Core/SparkEngine.cpp`
- `SparkEngine/Source/Core/GameplaySystemLifecycle.cpp`

**Reason:** Prevent new coupling to global service state and preserve `EngineContext`-based access.

---

### 3) Lateral subsystem include coupling

**Rule:** `SparkEngine/Source/Engine/<Domain>/...` may not directly include concrete internals from another `Engine/<OtherDomain>/...`.

**Reason:** Keep systems composable via components/events/contracts rather than hard references.

**Examples to avoid:**
- `Engine/AI/*` including `Engine/Audio/*` concrete internals
- `Engine/Gameplay/*` including `Engine/Physics/*` concrete internals

---

### 4) ECS direct sibling system lookup

**Rule:** ECS implementation `.cpp` files in `Engine/ECS/Systems` should not use `GetSystem()` sibling lookups for orchestration.

**Reason:** Enforces data-oriented execution ordering and avoids hidden inter-system control coupling.

---

### 5) Module ABI export and version contract

**Rule:** Every module `Source/Core/Main.cpp` must include:
- `CreateModule(...)` export
- `DestroyModule(...)` export
- `SPARK_SDK_VERSION` usage in module metadata

**Reason:** Ensures loader compatibility and explicit SDK ABI handshake.

## How to run

```bash
./tools/check-cross-utilization.sh
```

To include this in the standard validation flow:

```bash
./tools/validate-all.sh
```

## Notes

- This policy intentionally starts with high-signal checks.
- If you need an exception, update both:
  1) this document (rationale + scope), and
  2) `tools/check-cross-utilization.sh` (explicit allowlist rule).
- Keep exceptions minimal and traceable.
