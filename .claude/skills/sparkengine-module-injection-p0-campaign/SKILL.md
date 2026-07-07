---
name: sparkengine-module-injection-p0-campaign
description: >-
  Decision-gated campaign to finish and verify the SparkEngine "module EngineContext
  injection" P0 — the cross-DLL fix that makes EngineContext::Get() inside a game-module
  DLL resolve to the host engine's live context instead of a dead per-image copy.
  TRIGGER when: someone says the module DI / EngineContext injection P0 is "landed but inert",
  when module-side service-locator lookups (NetworkManager, GetNetwork) return null or a wrong
  singleton inside a GameModules DLL, when a HARDEN_FLEET / trilobite handoff lists
  "SparkModuleInjectEngineContext export still needed", or when a rebuilt module DLL must be
  proven to export the injection hook. DO NOT TRIGGER for general "how do I add a new module DLL"
  questions (use sparkengine-plugin-abi), for the ImGui or console injection hooks specifically
  (those already shipped), or for non-Windows builds (this whole seam is #if defined(_WIN32) only).
trilobite_compatible: true
trilobite_role: campaign
---

# SparkEngine — Module EngineContext Injection P0 Campaign

## Status as of 2026-07-07: this P0 is ALREADY COMPLETE at source level

Do not fabricate remaining work. Read this section first, run Gate 0, then decide.

The handoff doc `HARDEN_FLEET_HANDOFF.md` (branch `claude/harden-fleet`) lists this as
"P0 landed but INERT — finish first". That doc is **stale**. As verified against the live
tree on 2026-07-07, commit `52636dda` ("feat(harden-fleet): Phase 3 deferred cross-cutting
fixes") landed the missing pieces and is **in the current `HEAD` history**. Specifically:

| Piece the handoff said was missing | Reality now | Where |
|---|---|---|
| DLL-side export `SparkModuleInjectEngineContext` | **EXISTS** | `SparkSDK/Include/Spark/ModuleDllMain.h` lines ~88-91 |
| Host-side `GetProcAddress` lookup | **EXISTS**, symbol name matches exactly | `SparkEngine/Source/Core/ModuleManager.cpp` lines ~191-196 |
| `EngineContext::SetInjected` + `Get()` preference | **EXISTS** | decl `EngineContext.h:143`; def `EngineContext.cpp:29-43` |
| MMO DI (`MMOPlayerSystem.cpp`, `MMOWorldSetup.cpp`) | **FIXED** — both call `m_context->GetNetwork()` | see Gate 4 |
| Remove NetworkManager's dead per-module `GetInstance()` fallback | **PARTIAL** — routed through context first; static fallback still present (see "Open item") | `NetworkManager.cpp:130-145` |

**The only load-bearing gap left is the built binaries, not the source.** The shipped module
DLLs in `build/windows-release/bin/` are dated 2026-07-05 — *older than* the 2026-07-07 export
commit. They carry `SparkModuleInjectConsole` and `SparkModuleInjectImGui` but **not**
`SparkModuleInjectEngineContext`. Until you rebuild, the export is dead in the binaries even
though it is live in source. **Finishing this P0 today = rebuild + verify + wire-in check, not
new code.**

This skill doubles as documented precedent: the "if you find it NOT done" branches below record
exactly what the campaign *was*, so the next inert-P0 (there are more in the handoff's Deferred
list) can follow the same gated shape.

For general module-DLL ABI/conventions (how to author a *new* module, `SPARK_IMPLEMENT_MODULE`,
export naming), see the sibling skill **`sparkengine-plugin-abi`**. This skill is only the
finish-and-verify campaign for the EngineContext-injection P0.

---

## Vocabulary (defined once)

- **Module DLL** — a `GameModules/SparkGame*/` shared library, statically linked against
  `SparkEngineLib`. Because the link is static, every DLL gets its *own private copy* of engine
  globals (`g_engineContext`, `GImGui`, the console singleton).
- **Per-image copy** — a global that exists once *per loaded binary image* (the .exe and each
  .dll each have their own). The module's `g_engineContext` is null because only the host .exe
  ever populated *its* copy.
- **Injection hook** — an `extern "C" __declspec(dllexport)` function the host calls via
  `GetProcAddress` right after `LoadLibrary`, handing the DLL a raw pointer to a host-owned
  object so the DLL's per-image global points at the real thing.
- **Service locator** — `EngineContext::Get()->GetX()`. Inside a module this is the ONLY
  correct way to reach a subsystem; the DI fix is what makes it work there.
- **Inert / dead-in-binary** — code that is correct in source but not present in the compiled
  artifact because the artifact is stale.

---

## The campaign, gate by gate

Run every gate in order. Each gate has an **EXPECTED** observation. If the observation matches,
advance. If it does not, take the labeled **FAIL branch**.

### Gate 0 — Confirm which world you are in (30 seconds)

```bash
# From repo root D:\SparkEngine
git merge-base --is-ancestor 52636dda HEAD && echo "P0-COMMIT-PRESENT" || echo "P0-COMMIT-ABSENT"
```

- **EXPECTED (normal case):** `P0-COMMIT-PRESENT`. The source-level P0 is done; skip to Gate 3
  (rebuild) — Gates 1-2 are just confirmation you can spot-check.
- **FAIL branch (`P0-COMMIT-ABSENT`):** you are on a branch that predates the fix (e.g. a fork
  of the raw `claude/harden-fleet` checkpoint). The source work below is genuinely undone — do
  Gates 1-2 as *edits*, using the exact code blocks, then continue.

### Gate 1 — DLL-side export exists

```bash
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h
```

- **EXPECTED:** one hit at the `extern "C" __declspec(dllexport)` definition (~line 88). The
  body must be exactly:

  ```cpp
  extern "C" __declspec(dllexport) void SparkModuleInjectEngineContext(void* ctx)
  {
      EngineContext::SetInjected(static_cast<EngineContext*>(ctx));
  }
  ```

  It must sit **inside** the file's `#if defined(_WIN32)` block, and the file must
  `#include "Core/EngineContext.h"` above it (present at ~line 76). No new include of your own
  is needed — module TUs already have the engine `Source/` tree on their include path.

- **FAIL branch (missing):** add the block above immediately after the existing
  `SparkModuleInjectConsole` export (mirror its shape — same file already exports Console and
  ImGui hooks, so the pattern is right there). Add the `#include "Core/EngineContext.h"` if
  absent. Do **not** put this in a new file — it must compile into the module's DllMain TU
  (`GameModules/SparkGame*/Source/Core/Main.cpp`, which `#include <Spark/ModuleDllMain.h>` and
  calls `SPARK_IMPLEMENT_MODULE`). Putting the export elsewhere risks it being stripped or
  duplicated.

### Gate 2 — Host side calls the exact symbol name, and the setter exists

```bash
grep -n "SparkModuleInjectEngineContext\|InjectContextFn\|SetInjected\|g_injectedContext" \
  SparkEngine/Source/Core/ModuleManager.cpp SparkEngine/Source/Core/EngineContext.cpp
```

- **EXPECTED — host lookup** (`ModuleManager.cpp` ~191-196, inside `#ifdef _WIN32`, after the
  Console-inject block and before the ImGui-inject block):

  ```cpp
  using InjectContextFn = void (*)(void*);
  if (auto injectCtx = reinterpret_cast<InjectContextFn>(
          GetProcAddress(static_cast<HMODULE>(handle), "SparkModuleInjectEngineContext")))
  {
      injectCtx(EngineContext::Get());
  }
  ```

  The string literal `"SparkModuleInjectEngineContext"` must be **byte-identical** to the
  exported symbol from Gate 1 — a single typo makes `GetProcAddress` return null and the whole
  fix silently no-ops (the `if` just skips). This is the #1 way this class of bug hides.

- **EXPECTED — setter / preference** (`EngineContext.cpp`): a file-scope
  `static EngineContext* g_injectedContext = nullptr;`, `Get()` returns `g_injectedContext`
  when non-null else `g_engineContext.get()`, and `SetInjected(ctx)` assigns `g_injectedContext`.
  The injected pointer is deliberately **raw / non-owning** — module teardown must never free
  the host context.

- **FAIL branch (host lookup missing):** insert the lookup block between the existing Console
  and ImGui injection blocks in `ModuleManager::LoadModule`. **FAIL branch (setter missing):**
  add `static void SetInjected(EngineContext* ctx);` to `EngineContext.h` (public, next to
  `Get()`), the file-scope `g_injectedContext`, the `Get()` preference, and the setter def in
  `EngineContext.cpp`. Never make `g_injectedContext` a `unique_ptr`.

### Gate 3 — Rebuild so the export lands in the binary (the real remaining work)

This box is MSVC-via-vcvars with `CC=claude.exe` poisoning CMake — build through the pinned
one-call helper (per global CLAUDE.md), or a Dev Command Prompt. Run a **RAM preflight first**
(16 GB box, thrashes):

```powershell
powershell -NoProfile -File "C:\Users\natew\.claude\scripts\fleet-preflight.ps1"   # want GO
powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build/windows-release --config Release --target SparkGameMMO"
```

Only `SparkGameMMO` is needed to prove the fix; rebuild all modules before shipping. If the
`windows-release` build dir does not exist yet: `cmake --preset windows-release` first.

- **EXPECTED:** build succeeds, 0 errors. `build/windows-release/bin/SparkGameMMO.dll` mtime is
  now today, not 2026-07-05.
- **FAIL branch (build error in `ModuleDllMain.h`):** the most likely error is an unresolved
  `EngineContext::SetInjected` or an incomplete-type `EngineContext` — means the
  `#include "Core/EngineContext.h"` is missing or the module's include path lost the engine
  `Source/` root. Confirm the module target links `SparkEngineLib` and has `Source/` on its
  includes (it does by default via the shared CMake module setup — see `sparkengine-plugin-abi`).

### Gate 4 — Verify the export is actually in the DLL (the binary gate)

Two equivalent checks; pick by what your shell can reach.

**Canonical (needs vcvars / a Developer Command Prompt):**

```
dumpbin /exports build\windows-release\bin\SparkGameMMO.dll
```

**vcvars-free, git-bash-friendly (an ASCII scan of the export-name table — this is the check
this skill actually ran and trusts):**

```bash
for h in SparkModuleInjectEngineContext SparkModuleInjectConsole SparkModuleInjectImGui; do
  printf '%-32s ' "$h"; grep -acE "$h" build/windows-release/bin/SparkGameMMO.dll
done
```

- **EXPECTED (after Gate 3 rebuild):** all three names report `1`. In particular
  `SparkModuleInjectEngineContext` = `1`.
- **DIAGNOSTIC — what a STALE binary looks like:** on the un-rebuilt 2026-07-05 DLLs this exact
  scan reports `SparkModuleInjectEngineContext 0`, `SparkModuleInjectConsole 1`,
  `SparkModuleInjectImGui 1`. If you see the `0`, you skipped or under-scoped Gate 3 — rebuild
  the right target into the right `bin/`.
- **FAIL branch (still `0` after a clean rebuild):** the export is being stripped. Confirm it is
  compiled into the DllMain TU (the file that has `SPARK_IMPLEMENT_MODULE`), that it is not
  wrapped in an `#else`/non-`_WIN32` branch, and that no `.def` file or `/EXPORT` allowlist is
  overriding `__declspec(dllexport)` for this target.

### Gate 5 — Prove the injection is live end-to-end (behavioral gate)

The export existing is necessary but not sufficient; confirm the host actually calls it and a
module-side lookup resolves. Cheapest signal: the MMO module resolves networking through the
injected context.

```bash
grep -n "m_context->GetNetwork\|GetNetworkService\|GetInstance" \
  GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp \
  GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp
```

- **EXPECTED:** every networking access reads `m_context->GetNetwork()` (guarded
  `m_context ? m_context->GetNetwork() : nullptr`), with a comment noting "through the injected
  engine context, not the global singleton". Verified today: `MMOPlayerSystem.cpp` lines ~49 and
  ~166; `MMOWorldSetup.cpp` lines ~239, ~288, ~334. Any surviving
  `NetworkManager::GetInstance()` in *networking* paths here is the bug.
- **Runtime signal (optional):** launch the engine with the MMO module and watch the log — with
  the fix, module-side `GetNetwork()` returns the host's `NetworkManager` (the same instance the
  host registered via `SetNetwork`). Without it, module lookups return null or a second, empty
  singleton and MMO replication is silently dead.
- **FAIL branch:** a networking call still uses `GetInstance()` — replace it with
  `m_context->GetNetwork()` (thread the `Spark::IEngineContext* m_context` captured in the
  system's `Initialize(context)`; both MMO systems already store it).

### Known-wrong paths — do not take these

- **Do NOT** make the module reach a subsystem by calling `SubsystemName::GetInstance()` and
  hoping the static local is shared. Statically-linked singletons are **per-image**; the module's
  copy is a different object from the host's. This is the exact bug the P0 fixes. The only
  cross-DLL-correct path is `m_context->GetX()` (or `EngineContext::Get()->GetX()` once injected).
- **Do NOT** store the injected `EngineContext*` in a `unique_ptr` or otherwise take ownership.
  The module must never free the host context; teardown/`FreeLibrary` would double-free.
- **Do NOT** rename the export or the `GetProcAddress` string to "fix" a null lookup without
  checking both ends match byte-for-byte first. A mismatch degrades to a silent no-op, not a
  crash — so it looks like "the fix didn't work" rather than "I typo'd".
- **Do NOT** try to verify the export against the stale `build/.../bin/*.dll` without rebuilding.
  The pre-2026-07-07 binaries legitimately lack the symbol; a `0` there means "old binary", not
  "broken source".
- **Do NOT** port this seam to Linux/macOS as-is. The entire mechanism is `#if defined(_WIN32)`
  because the per-image-global problem is specific to static-linking each DLL on Windows; POSIX
  `dlopen` with a shared engine .so does not reproduce it. Leave the non-Windows path alone.

### Open item (honest, not oversold)

The handoff asked to **remove** NetworkManager's "dead per-module `GetInstance()` fallback".
Current `NetworkManager::GetInstance()` (`NetworkManager.cpp:130-145`) does the *intended* thing
— it resolves through `EngineContext::Get()->GetNetworkService()` first — but it **still keeps**
the `static NetworkManager instance;` as a last-resort fallback rather than deleting it. So the
literal handoff instruction ("remove the fallback") is only **partially** satisfied: the fallback
is now unreachable whenever a context is injected, but it was not deleted. Treat full removal as a
`candidate` cleanup, not a done item — and note that some non-module/test call sites may still
depend on the fallback existing, so do not delete it without checking callers
(`grep -rn "NetworkManager::GetInstance" SparkEngine Tests GameModules`).

Separately: `MMOWorldSetup.cpp` still calls `Spark::Streaming::SeamlessAreaManager::GetInstance()`
(lines ~135, ~230). That is a **streaming** singleton, outside this networking-DI P0's scope — do
not conflate it with the P0. If it needs the same treatment, that is its own task.

---

## Validation and promotion — route through this project's change control

This repo enforces its own gates (see `D:\SparkEngine\CLAUDE.md`). Promote the finished P0 through
them; do not hand-wave past CI.

0. **Sync first, then work (CLAUDE.md "Git Sync Workflow"):** the upstream branch is `Working`,
   not `main`. Before you touch anything and again before you commit/push, `git fetch origin Working`
   and `git rebase origin/Working` if behind — **never commit or push while behind the base branch.**
   **Concurrency caveat:** a hardening fleet may be editing the very files this P0 touches on branch
   `claude/harden-fleet` (`SparkSDK/Include/Spark/ModuleDllMain.h`,
   `SparkEngine/Source/Core/ModuleManager.cpp`, `EngineContext.cpp`, and the MMO files). Coordinate
   and rebase — do **not** race concurrent edits into those files, and use targeted `git add` paths
   (never `git add -A`) while a fleet is writing.
1. **Wiring rule (CLAUDE.md "Wiring Things In"):** the fix is only "done" if the export is
   *called*. Gates 2 and 5 are your wiring proof — a system that exists but is never invoked is
   "worse than not existing".
2. **Format (CI `check-format` is blocking):** any C++ you touched must pass clang-format:
   ```bash
   clang-format --dry-run --Werror SparkSDK/Include/Spark/ModuleDllMain.h \
     SparkEngine/Source/Core/ModuleManager.cpp SparkEngine/Source/Core/EngineContext.cpp \
     GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp
   ```
3. **Build both compilers locally if you edited source** — CI runs GCC + Clang + MSVC; the Linux
   jobs will still compile the file even though the injection body is `_WIN32`-gated (the gate
   must keep them clean). `cmake --preset linux-gcc-release && cmake --build build --config Release`.
4. **Tests:** `cd build && ctest --output-on-failure`. There is no dedicated injection unit test
   today (cross-DLL load is hard to unit-test); the behavioral proof is Gate 5 + a live module
   launch. If you add a test, register it (unregistered `Test*.cpp` never compile — see the
   handoff's "Tests wiring" note).
5. **Docs:** if you record this as solved, add/update a page under `wiki/advanced/` (non-obvious
   codebase fact) per CLAUDE.md's wiki rules, then `docs/sync-wiki.sh sync`. Do not create a
   parallel note outside the wiki.
6. **PR + CI poll:** after pushing, `gh pr checks --watch --fail-fast`; `check-format`,
   `build-windows-vs2022`, and the Linux builds are blockers.

## Provenance and maintenance

Verified against the live tree on **2026-07-07** (commit `52636dda` in `HEAD`; working tree clean
for all five files). Re-verify with:

```bash
# Is the P0 source commit still in history?
git merge-base --is-ancestor 52636dda HEAD && echo present || echo absent
# Export defined in the SDK header?
grep -n "SparkModuleInjectEngineContext" SparkSDK/Include/Spark/ModuleDllMain.h
# Host looks up the exact symbol?
grep -n "SparkModuleInjectEngineContext" SparkEngine/Source/Core/ModuleManager.cpp
# Setter + Get() preference present?
grep -n "SetInjected\|g_injectedContext" SparkEngine/Source/Core/EngineContext.cpp
# MMO DI uses the context, not GetInstance?
grep -n "m_context->GetNetwork\|GetInstance" \
  GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.cpp \
  GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp
# Is the export actually in the built DLL yet? (0 = stale binary, rebuild)
grep -acE "SparkModuleInjectEngineContext" build/windows-release/bin/SparkGameMMO.dll
```

Lint this skill: `powershell -NoProfile -File C:\Users\natew\.claude\skills\skill-linter\scripts\skill-lint.ps1 -Path D:\SparkEngine\.claude\skills\sparkengine-module-injection-p0-campaign`
