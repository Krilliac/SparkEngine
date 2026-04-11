# Engine next-steps — Phase V (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Activate `Spark::Graphics::ShaderDiskCache` — the persistent
on-disk shader cache from `Graphics/ShaderDiskCache.h`. Second phase of
Theme 3A (shader hot-reload surface) from the Phase U+ plan.

---

## Summary

Wired the `Spark::Graphics::ShaderDiskCache` class from
`Graphics/ShaderDiskCache.h` into the `Shader` lifecycle on both
Windows and Linux branches. Prior to this phase the class had zero
production call sites — the only test file
(`Tests/TestShaderDiskCache.cpp`) exercised a local reimplementation
(`TestShaderBlob` / `TestShaderDiskCache` in an anonymous namespace),
exactly the "audit-claimed coverage" anti-pattern Phase O / P / Q /
playbook step 6 warns about.

After this phase:

- A process-wide singleton `Spark::Graphics::GetShaderDiskCache()`
  returns a Meyers singleton `ShaderDiskCache` instance that every
  translation unit shares.
- `Shader::Initialize` (both branches) calls
  `GetShaderDiskCache().Initialize("ShaderCache")` on first use so the
  cache directory is materialised under the working directory before
  any shader is compiled.
- Linux `Shader::LoadShaderFromSource` consults the cache before
  calling `Spark::RHI::CompileShader` — on a hit the RHI compile is
  skipped and the cached bytecode is reused; on a miss the compile
  result is stored back into the cache for future runs.
- Windows `Shader::LoadShaderFromSource` is **store-only** for now:
  after a successful `D3DCompile`, the DXBC bytecode is copied into
  the shared cache so a subsequent run on any platform reusing the
  same `ShaderCache/` directory can pick it up. Lookup on Windows is
  deferred — `CreateInputLayout` needs an `ID3DBlob*` so the Windows
  hit path requires wrapping cached bytes with `D3DCreateBlob`; that
  expansion lands in a follow-up phase.

## Files touched (code)

- `SparkEngine/Source/Graphics/ShaderDiskCache.h`
  - Added inline `Spark::Graphics::GetShaderDiskCache()` singleton
    accessor at the bottom of the namespace. Meyers static local —
    safe across translation units with vague linkage.

- `SparkEngine/Source/Graphics/Shader.cpp`
  - Added `#include "ShaderDiskCache.h"` on both Windows and Linux
    include blocks.
  - Windows + Linux `Shader::Initialize`: lazy
    `GetShaderDiskCache().Initialize(std::filesystem::path("ShaderCache"))`
    on first use. Guarded by `IsInitialized()` so subsequent Shader
    instances skip the re-init but still share the same cache.

- `SparkEngine/Source/Graphics/ShaderCompilation.cpp`
  - Added `#include "ShaderDiskCache.h"` on both Windows and Linux
    include blocks.
  - Added Linux-local helpers `ShaderTypeToGraphicsStage` and
    `MakeCacheSource` for building the cache key from
    `(ShaderType, ShaderCompilationFlags, hlslCode)`.
  - **Linux `LoadShaderFromSource`:** wrap the existing RHI compile
    call with cache lookup/store. Target pinned to `ShaderTarget::DXBC`
    as the canonical cache key until the RHI cross-compile pipeline
    wires a per-backend target selector. Metrics bookkeeping is
    unchanged so cache hits still bump `compiledShaders` and
    `shaderMemoryUsage` — from the metrics point of view a hit looks
    like a very fast compile.
  - **Windows `LoadShaderFromSource`:** store-only on
    `D3DCompile` success. The cached source struct is built inline
    because the Linux helper functions live inside the `#else` branch
    and aren't visible here.

## Files touched (tests)

- `Tests/TestShaderDiskCachePhaseV.cpp` (new) — 15 tests against the
  real `Spark::Graphics::ShaderDiskCache` class + the new
  `GetShaderDiskCache()` accessor. Every test resets the singleton
  first via `ResetDiskCache()` (calls `Shutdown` on the accessor).

  Coverage:
  1. `SingletonReturnsSameInstance` — singleton accessor returns a
     stable reference across calls (invariant for cross-TU sharing).
  2. `InitializeCreatesDirectory` — Initialize creates the cache root.
  3. `ShutdownClearsInitializedFlag` — Shutdown toggles `IsInitialized`.
  4. `InitializeIdempotent` — calling Initialize twice on the same
     directory does not crash or double-register.
  5. `StoreLookupRoundTrip` — blob bytecode, stage, target, success
     survive Store + Lookup.
  6. `DifferentSourceMisses` — hash changes when HLSL source changes.
  7. `DifferentDefinesMisses` — hash changes when preprocessor defines
     change (keyword toggle = new cache entry).
  8. `DifferentStageMisses` — VS and PS of identical code hash differently.
  9. `DifferentTargetMisses` — DXBC and SPIRV of identical source hash
     differently.
  10. `LookupOnEmptyReturnsNullopt` — lookup on empty cache returns
     nullopt (not a stale blob).
  11. `LookupOnShutdownReturnsNullopt` — post-Shutdown lookup refuses
     even when the file is still on disk.
  12. `StoreFailedBlobIgnored` — `Store` refuses `success = false`
     blobs.
  13. `StoreEmptyBytecodeIgnored` — `Store` refuses empty-bytes blobs.
  14. `EntryCountAndDiskUsage` — counters track stores.
  15. `ClearRemovesAllEntries` — `Clear` removes all `.blob` files but
     keeps `IsInitialized` true.
  16. (Named `PersistsAcrossShutdownInitialize`) — end-to-end
     persistence: Store, Shutdown, Initialize (same dir), Lookup
     returns the stored blob. This is the invariant that lets engine
     restarts reuse cached bytecode.

- `Tests/CMakeLists.txt` — added `TestShaderDiskCachePhaseV.cpp`.

## Activation verification

After Phase V:

- `Spark::Graphics::GetShaderDiskCache()` has production call sites
  in `Shader::Initialize` (both branches) and
  `Shader::LoadShaderFromSource` (both branches — Linux
  lookup/store, Windows store-only).
- `ShaderDiskCache::Initialize` is lifecycle-owned by Shader and
  materialises `ShaderCache/` under the working directory on first
  `Shader::Initialize` call.
- The orphan count for Theme 3A shader hot-reload triplet drops from
  2 (post-Phase-U) to 1 — only `ShaderCrossCompiler` remains for
  Phase W.
- Existing `TestShaderDiskCache.cpp` tests still pass (they never
  touched the real class; they exercised a local reimplementation).

## Playbook notes for future phases

1. **Meyers singleton via inline free function in the header** is a
   low-risk activation pattern when the orphan class is not itself a
   singleton. The `GetShaderDiskCache()` inline free function lives
   in the header alongside the class definition; every TU gets the
   same static local by way of vague linkage. Phase U used the same
   `GetInstance()` approach because `ShaderHotReload` had a member
   static already. Both are valid — pick whichever requires fewer
   header edits.

2. **Cache key must be stable across compiler runs.** The Phase V
   wire-up pins `ShaderTarget::DXBC` as the canonical cache target.
   This is intentional — the `Spark::RHI::CompileShader` path
   currently produces DXBC on Windows and DXBC-shaped bytecode on
   Linux (via the RHI fallback), so a single target covers both. A
   future phase that wires real per-backend compile paths will need
   to thread the `RHI::GraphicsBackend` selection through the cache
   key.

3. **Windows hit path is harder than Linux.** The D3D11 input-layout
   reflection path (`CreateInputLayout(ID3DBlob*, ...)`) consumes a
   `ID3DBlob*`, not raw bytes. On a cache hit we'd need to wrap
   cached bytes with `D3DCreateBlob`. That's achievable but fiddly
   and out of scope for Phase V — the Windows branch is store-only
   so the cache still populates for subsequent Linux runs sharing the
   same `ShaderCache/` directory. Phase W (`ShaderCrossCompiler`) or
   a dedicated follow-up can finish the Windows hit path.

4. **Metrics semantics unchanged.** Cache hits still bump
   `compiledShaders` and `shaderMemoryUsage` in the metrics struct —
   the per-compile consumer's view of the system is the same. Cache
   hits simply complete in microseconds instead of milliseconds.

5. **Test isolation matters for singletons.** Every Phase V test
   starts with `ResetDiskCache()` (which calls `Shutdown`) to flush
   any residual state from earlier test files. `Store`/`Lookup` are
   filesystem-backed so concurrent test runs against the same
   process image must use disjoint cache directories — each test
   builds its own `std::filesystem::temp_directory_path() /
   spark_phaseV_*` subdir.

6. **The test for persistence is critical.** Without a test that
   verifies Store → Shutdown → Initialize (same dir) → Lookup
   succeeds, the cache is theoretically-persistent but not
   engineering-persistent. Phase V's `PersistsAcrossShutdownInitialize`
   test locks this down.

## Cross-references

- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Previous phase: [engine-next-steps-phase-u-2026-04-11.md](engine-next-steps-phase-u-2026-04-11.md) — ShaderHotReload activation
- Next phase: Phase W (`ShaderCrossCompiler`) — final Theme 3A orphan
