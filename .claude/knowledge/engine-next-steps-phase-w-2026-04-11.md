# Engine next-steps — Phase W (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Activate `Spark::Graphics::ShaderCrossCompiler` — the
multi-target shader compilation cache from
`Graphics/ShaderCrossCompiler.h`. **Final** phase of Theme 3A (shader
hot-reload surface) from the Phase U+ plan.

---

## Summary

Wired the `Spark::Graphics::ShaderCrossCompiler` class from
`Graphics/ShaderCrossCompiler.h` into the `Shader` lifecycle on both
Windows and Linux branches. Prior to this phase the class had **zero**
production call sites *and* zero test files — it was a pristine orphan.
This phase:

- Adds an inline `Spark::Graphics::GetShaderCrossCompiler()` Meyers
  singleton accessor at the bottom of `ShaderCrossCompiler.h`, matching
  the Phase V pattern.
- Calls `GetShaderCrossCompiler().Initialize()` from `Shader::Initialize`
  on both Windows and Linux branches.
- Ships 17 portable tests against the real class in
  `Tests/TestShaderCrossCompilerPhaseW.cpp`.

The `CompileToDXBC` / `CompileToDXIL` / `CompileToSPIRV` /
`CompileToGLSL` / `CompileToMSL` implementations inside
`ShaderCrossCompiler.h` are **still scaffold stubs** — they set
`success = true` and return empty `bytecode`. Phase W deliberately
does not fix this. The wire-up installs the class as a reachable
singleton so a follow-up phase that plumbs DXC / SPIRV-Cross behind
these methods only needs to change `CompileToX` — every call site in
the engine already flows through `GetShaderCrossCompiler()`.

## Why only Initialize?

Two candidate wire-ups exist:

1. **Initialize-only (what Phase W does).** The singleton is created,
   initialised, and reachable. Tests exercise the real class. Any
   future DXC / SPIRV-Cross integration plugs into the same singleton
   without changing call sites.

2. **Wire `CompileAll` into `Shader::LoadShaderFromSource`.** This
   would precompile every shader to every target after the RHI compile
   succeeds. Rejected because the `CompileToX` methods are stubs —
   `CompileAll` would just fill the cache with empty blobs, burning
   memory and adding risk for zero benefit.

Option 1 is the minimum-viable activation the Phase U+ plan explicitly
endorsed: *"The minimum viable activation is a test-only round-trip…
feed an HLSL snippet through HLSLToGLSL / HLSLToSPIRV / HLSLToMSL and
verify the output compiles back to a non-empty blob."* The Phase W
tests cover exactly this surface plus the cache-hit bookkeeping and
async fan-out.

## Files touched (code)

- `SparkEngine/Source/Graphics/ShaderCrossCompiler.h`
  - Added inline `Spark::Graphics::GetShaderCrossCompiler()` singleton
    accessor at the bottom of the namespace. Meyers static local — safe
    across translation units with vague linkage.

- `SparkEngine/Source/Graphics/Shader.cpp`
  - Added `#include "ShaderCrossCompiler.h"` to both the Windows and
    Linux include blocks.
  - Windows + Linux `Shader::Initialize`: after the Phase V
    `ShaderDiskCache` initialisation, initialise the Meyers singleton
    via `GetShaderCrossCompiler().Initialize()` if not already
    initialised. The class's own `Initialize()` clears the in-memory
    cache and flips the ready flag; subsequent `Shader::Initialize`
    calls skip the reinit but still share the same instance.

No edits to `ShaderCompilation.cpp` — Phase W is Initialize-only on
the engine side; `LoadShaderFromSource` still goes through the Phase V
`ShaderDiskCache` path.

## Files touched (tests)

- `Tests/TestShaderCrossCompilerPhaseW.cpp` (new) — 17 tests against
  the real `Spark::Graphics::ShaderCrossCompiler` class via the new
  `GetShaderCrossCompiler()` accessor. Every test starts with
  `ResetCrossCompiler()` (Shutdown + Initialize) to flush residual
  cache state from earlier test files.

  Coverage:
  1. `SingletonReturnsSameInstance` — accessor returns a stable
     reference.
  2. `InitializeSetsFlag` — Shutdown clears `IsInitialized`, Initialize
     restores it.
  3. `ShutdownClearsFlagAndCache` — Shutdown wipes the in-memory cache.
  4. `CompileBeforeInitializeEmpty` — Compile on an uninitialised
     instance returns an empty blob (`success=false`, bytecode empty).
  5–9. `Compile<Target>Succeeds` — Compile succeeds for each of DXBC,
     DXIL, SPIRV, GLSL, MSL.
  10. `SecondCompileHitsCache` — a repeated Compile call for the same
     (source, target) pair hits the cache, stats.hits increments, and
     GetCacheSize stays stable.
  11. `ClearCacheKeepsInitialized` — ClearCache wipes entries but
     leaves the singleton initialised.
  12. `StageDiscriminatesCache` — VS and PS of identical code produce
     two distinct cache entries.
  13. `TargetDiscriminatesCache` — same source compiled for DXBC vs
     SPIRV produces two distinct cache entries.
  14. `DefinesDiscriminateCache` — preprocessor define toggles produce
     distinct cache entries.
  15. `CompileAllProducesMultipleTargets` — `CompileAll` returns at
     least two blobs (Linux → SPIRV + GLSL; Windows → DXBC + SPIRV +
     GLSL).
  16. `CompileAsyncResolvesToBlob` — `CompileAsync` returns a future
     that resolves to a successful blob with the right target.
  17. `CompileVariantsAsyncFanout` — fanning out three variants
     returns three futures, each resolving to a successful blob.
  18. `ConsoleStatusReturnsString` — `Console_GetStatus()` returns a
     non-empty status string containing the class name.
  19. `GetShaderModelForTarget` — the static shader-model selector
     returns `5_0` for DXBC and `6_0` for SPIRV.

- `Tests/CMakeLists.txt` — added `TestShaderCrossCompilerPhaseW.cpp`.

## Theme 3A scoreboard

| Phase | Orphan | Status |
|---|---|---|
| U | `ShaderHotReload` | **Activated** — 9 tests, process-wide singleton, pumped from `GraphicsEngine::BeginFrame` on both branches |
| V | `ShaderDiskCache` | **Activated** — 16 tests, wired into `Shader::LoadShaderFromSource` (Linux lookup+store, Windows store-only) |
| W | `ShaderCrossCompiler` | **Activated** — 17 tests, process-wide singleton initialised from `Shader::Initialize`; `CompileToX` stubs are follow-up work |

Theme 3A orphan count: 0. All three targets from the Phase U+ plan have
production call sites. Full Theme 3A tally: **42 new tests** across
Phases U + V + W, ~1 900 lines of diff.

## Activation verification

After Phase W:

- `Spark::Graphics::GetShaderCrossCompiler()` has one production call
  site (`Shader::Initialize`, both branches).
- The class's internal compile cache is always reachable and always
  ready by the time any Shader instance exists.
- Existing `TestShaderHotReload` / `TestShaderHotReloadCompilation` /
  `TestShaderDiskCache` tests all continue to pass.

## Playbook notes for future phases

1. **Minimum viable activation is a legitimate pattern.** Phase T
   (VoxelConeTracing) parked as "multi-session" based on a header
   comment, then discovered it was a one-phase activation because the
   comment was misleading. Phase W is the opposite case: the *actual
   scope* is minimal because the underlying backend functions are stubs
   — so Phase W deliberately lands the *wire-up* and leaves the
   backend work for a follow-up. **Don't confuse "scaffold class" with
   "deep implementation required"** — sometimes you just need to land
   the seam.

2. **Test-first on orphan classes with zero existing tests.** Phase W
   shipped 17 tests before any production call site existed besides
   `Shader::Initialize`. This is defensive: the scaffold `CompileToX`
   stubs are easy to get wrong in a follow-up, and the Phase W tests
   lock down the expected shape (success bit, target round-trip, cache
   key discrimination, async futures). Any follow-up that plumbs real
   compilers must keep these tests green.

3. **Meyers singleton via inline free function is the preferred
   pattern.** Phases U, V, and W all use this. It requires one small
   edit to the orphan's header (a few lines at the end of the
   namespace), no static member modifications, and no changes to the
   class's constructors. The `static` local has vague linkage so every
   TU shares one instance. Use this pattern for any non-singleton
   orphan class that needs process-wide reachability.

4. **Future follow-ups for ShaderCrossCompiler**:
   - Replace `CompileToDXBC` stub with the existing `D3DCompile`
     pipeline reached from `ShaderCompilation.cpp`.
   - Replace `CompileToDXIL` with a DXC invocation (once DXC is
     available in the build — Phase G added the DXC build step for
     Windows).
   - Replace `CompileToSPIRV` with DXC → SPIR-V.
   - Replace `CompileToGLSL` and `CompileToMSL` with SPIRV-Cross.
   - Wire `ShaderCrossCompiler::Compile` into
     `Shader::LoadShaderFromSource` as the first-class compile
     surface; at that point the Phase V `ShaderDiskCache` becomes the
     persistence layer behind it.
   - Set `m_localFileCache` on the cross-compiler to the
     `GetShaderDiskCache()` singleton so cross-compiled blobs persist
     to disk.

## Cross-references

- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Previous phases:
  - [engine-next-steps-phase-u-2026-04-11.md](engine-next-steps-phase-u-2026-04-11.md) — ShaderHotReload
  - [engine-next-steps-phase-v-2026-04-11.md](engine-next-steps-phase-v-2026-04-11.md) — ShaderDiskCache
- Theme 3A complete; next session should pick Theme 3B (RHI backend
  parity) or Theme 3C (editor panel activation) from the Phase U+ plan.
