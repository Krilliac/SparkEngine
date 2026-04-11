# Engine next-steps — Phase U+ plan (2026-04-11)

**Type:** Plan
**Status:** Deferred — to be executed in a future session
**Scope:** Post-Tier-2-graphics-orphan roadmap. Direction 3 from the
Phase T knowledge entry: *start a new theme* now that the
Tier 2 graphics orphan pool (Phases I→T) is exhausted.

---

## Context

Phases I→T (thirteen commits) activated 18 Tier 2 graphics orphans
across the `PostProcessingPipeline`, `LODManager`, `SceneRenderer`,
`LightingSystem`, `Shader`, `MaterialSystem`, `GraphicsEngine`,
and `UISystem` surfaces. With that pool exhausted, the Phase T
knowledge entry sketched three continuation directions:

1. Tier 3 / Tier 4 orphans from the non-Graphics audit sections.
2. Quality improvements on I→T "lifecycle only" activations.
3. Start a new roadmap theme.

The user picked **direction 3** but deferred execution to a later
session. This document is the deferred plan.

Three candidate themes are sketched below, in priority order. A
future session can pick one and execute with the I→T playbook:
one orphan per phase, portable tests against the real class, clean
build + format, commit + push, update the knowledge index.

---

## Theme 3A — Shader hot-reload surface (PRIORITY)

**Why first:** Three shader-subsystem orphans already exist in the
Graphics folder and have zero production call sites. They form a
natural triplet — hot-reload notifier, disk cache, cross-compiler
— that a real runtime shader pipeline would use together. Phase O
activated `ShaderVariantSystem` as a per-`Shader` member, and
these three fit the same surface.

### Target orphans

| File | Lines | Classes | Integration surface |
|---|---|---|---|
| `Graphics/ShaderHotReload.h` | 549 | `ShaderHotReload` (singleton), watch registration, reload callbacks | Per-engine singleton, initialised from `Shader::Initialize`, polled each frame or via filesystem watch |
| `Graphics/ShaderDiskCache.h` | 111 | `ShaderDiskCache` (singleton?), compile-blob persistence | Owned by `Shader` alongside the Phase O `ShaderVariantSystem`; consulted by `Shader::LoadVertexShader` / `LoadPixelShader` to skip recompilation when the source hash matches the cache |
| `Graphics/ShaderCrossCompiler.h` | 379 | 4 classes — HLSL ↔ GLSL / SPIR-V / MSL translation | Stateless utility; activation is "real caller that cross-compiles at least one shader," which could be a new Vulkan/OpenGL backend path or a test-only round-trip |

All three are portable (or mostly portable — spot-check the
`ShaderHotReload.h` file-watcher API for Windows-only file APIs
before wiring into a shared Initialize path).

### Phase U proposal: `ShaderHotReload`

**Activation pattern:** Follow Phase O (`ShaderVariantSystem`).
Add a singleton touch in `Shader::Initialize` on both Windows
and Linux branches. Register a file watch for every `.hlsl` /
`.hlsli` path that `Shader::LoadVertexShader` /
`LoadPixelShader` processes. `Shader` polls the hot-reload
system each frame (or on a deferred queue) and triggers a
recompile when a watched file changes on disk.

**Test coverage:** Tests against the real `ShaderHotReload`
class covering singleton liveness, watch registration /
deregistration, change detection (via a `TouchFileMTime(...)`
test helper that bumps a file's modification time without
actually editing it), callback invocation, and concurrent
watch safety.

**Expected size:** ~20 portable tests + minimal Shader.cpp
wiring. Knowledge entry ~350 lines.

### Phase V proposal: `ShaderDiskCache`

**Activation pattern:** Per-`Shader` member following the
Phase O pattern. Compute a content hash (FNV-1a? SHA-1?) of
the HLSL source in `Shader::LoadShaderFromSource` and query the
cache before calling `CompileShaderFromFile`. On cache hit,
skip the DXC/FXC compile and bind the cached blob directly.
On cache miss, compile + store. Cache persists to a local file
(e.g. `build/shader-cache/`) so CI and local builds can share
the cache across runs.

**Test coverage:** Tests against the real class covering hash
computation determinism, store + retrieve round-trip, hash
mismatch invalidation, disk persistence (tempfile-based),
version stamping (cache format upgrade path), concurrent
access if relevant.

**Expected size:** ~15 portable tests + modest `Shader.cpp`
wiring to route through the cache first. Knowledge entry
~300 lines.

### Phase W proposal: `ShaderCrossCompiler`

**Activation pattern:** More complex — cross-compilation only
matters when there's a target backend to compile *to*. The
minimum viable activation is a test-only round-trip: feed an
HLSL snippet through `HLSLToGLSL` / `HLSLToSPIRV` / `HLSLToMSL`
and verify the output compiles back to a non-empty blob. A
"real" activation would wire this into the Vulkan / OpenGL /
Metal RHI backends so shaders compile once in HLSL and run
everywhere.

**Test coverage:** Tests against each cross-compile target
against a small known HLSL input. No backend wiring in the
first pass — defer that to Theme 3B (RHI backend parity).

**Expected size:** ~10 tests, no production wiring beyond a
compile-time `[[maybe_unused]]` touch to verify the classes
link. Knowledge entry ~200 lines — small phase, low risk.

**Total Theme 3A effort:** Three phases (U, V, W), ~45 tests,
~3 new knowledge entries, ~1200 lines of diff.

---

## Theme 3B — RHI backend parity

**Why second:** SparkEngine ships 6 RHI backends (D3D11, D3D12,
Vulkan, OpenGL, Metal, NullRHIDevice) but Phases I→T all wired
exclusively into the D3D11 primary path. The Graphics folder
contains RHI-specific orphans (e.g. `RHIHandlePool.h`,
`TransientBufferAllocator.h`) that each backend could adopt for
parity with the D3D11 activation.

### Target orphans

| File | Lines | Classes | Integration surface |
|---|---|---|---|
| `Graphics/RHI/RHIHandlePool.h` | ~150 | `RHIHandlePool<T>` | Generic handle pool template — used by each backend for command-list / framebuffer / descriptor-set lifetimes. Activation: instantiate in `VulkanDevice`, `D3D12Device`, `OpenGLDevice` to match the `D3D11Device` usage |
| `Graphics/RHI/TransientBufferAllocator.h` | ~200 | `TransientBufferAllocator` | Per-frame transient buffer allocator (similar to Phase N's `ConstantBufferRing` but for vertex/index/upload buffers). Wire into each backend's per-frame `BeginFrame` |

### Surface inspection needed

Before planning concrete phases, a future session should:

1. **Grep** each orphan for existing usage (`RHIHandlePool<` /
   `TransientBufferAllocator`). Phase L caught a similar gotcha
   on `BVHAccelerator` — "declared but never called" members.
2. **Read** `D3D11Device.h` / `D3D12Device.h` /
   `VulkanDevice.h` / `OpenGLDevice.h` to find the parallel
   member slot each backend provides.
3. **Decide** whether the orphan should live in the abstract
   `RHIDevice.h` base class (portable across all backends) or
   as per-backend specialisation.

### Phase shape

Each backend is a separate phase:

- **Phase X:** `RHIHandlePool` activated in `VulkanDevice`
- **Phase Y:** `RHIHandlePool` activated in `D3D12Device`
- **Phase Z:** `RHIHandlePool` activated in `OpenGLDevice` /
  `NullRHIDevice`
- **Phase AA:** `TransientBufferAllocator` activated in all
  backends (single phase because the integration pattern is
  small)

**Caveat:** RHI backend code is harder to test on Linux CI
because `D3D12Device` and `VulkanDevice` may need real GPU
contexts. Plan to follow the Phase L / Phase Q precedent of
gating the integration tests behind `#ifdef SPARK_PLATFORM_WINDOWS`
(for D3D12) or `#ifdef SPARK_HAS_VULKAN` (for Vulkan), and writing
portable tests against the abstract `RHIHandlePool<T>` template
directly when possible.

**Total Theme 3B effort:** ~4 phases, ~60 tests (many
Windows-only), ~4 new knowledge entries. Significantly more
scope than Theme 3A because it touches four backends.

---

## Theme 3C — Editor panel activation

**Why third:** Several editor panels in `SparkEditor/Source/Panels/`
were catalogued in the April 10 audit as "header-only editor
infrastructure that exists but does nothing useful yet."
Phases B / G already cleaned up some of them (`SelectionManager`,
`EditorLayoutManager`, `EditorWindowManager`, `CSGEditorPanel`,
`NetworkDebugPanel`). This theme revisits the Panels/ folder
after the Tier 2 graphics surge to clean up the rest.

### Surface inspection needed

Before planning, a future session should:

1. Grep `SparkEditor/Source/Panels/` for classes that never
   appear in `EditorPanelFactory::Register...` call sites.
2. Distinguish "header-only with no .cpp" (likely delete or
   wire) from "class with .cpp but never registered" (wire).
3. Check the April 10 audit's Tier 4 section
   (`stub-and-abandoned-features-2026-04-10.md`) for the
   remaining entries that Phases A–G didn't touch.

### Likely targets

`SparkEditor/Source/Panels/` has 30+ `.h` files. A future session
should walk `EditorPanelFactory.cpp` and cross-reference against
the directory listing to find the unregistered ones. The Phase R
wire-or-delete audit pattern applies directly: grep, decide,
wire or document.

**Total Theme 3C effort:** Unknown until the survey is done.
Rough estimate: 3–6 phases depending on how many panels are
still orphaned.

---

## Recommended execution order

1. **Phase U — `ShaderHotReload`** (highest value, smallest risk)
2. **Phase V — `ShaderDiskCache`** (builds on Phase U)
3. **Phase W — `ShaderCrossCompiler`** (test-only activation,
   small phase)
4. **Survey then start Theme 3B or 3C** depending on which has
   a larger residual orphan count.

Rationale: Theme 3A is the tightest fit — three related orphans
on one subsystem surface, all mostly portable, clear activation
pattern matching Phase O. Start there, ship three phases, then
re-evaluate the remaining themes.

---

## Playbook (reusable across themes)

Every phase in the I→T roadmap followed the same rhythm. Future
phases should too:

1. **Source read.** Read the orphan header end-to-end before
   assuming scope. Phase S parked VCT as "multi-session" based
   on the header comment; Phase T discovered it was a one-
   phase activation because the comment was misleading. **Never
   trust audit scope claims without source verification.**

2. **Portability check.** `#ifdef SPARK_PLATFORM_WINDOWS` gates?
   D3D11 includes? `ComPtr` members? If none: wire outside the
   Windows guard and test on every platform. If yes: wire
   inside and gate the tests with `#ifdef`.

3. **Namespace qualification.** Classes in the global `Spark::`
   namespace (`Shader`, `LightingSystem`, `MaterialSystem`,
   `UISystem`) need explicit `Spark::Graphics::` qualification
   for orphan-type members. Phases M / O / P / R all hit this.

4. **Lifecycle hooks on both branches.** Files with duplicated
   Windows + Linux implementations (`LightingSystem.cpp`,
   `Shader.cpp`, `GraphicsEngine.cpp`) need the lifecycle hooks
   mirrored on both branches or the Linux build breaks with
   unresolved symbols. Phase Q / S / T each caught this.

5. **Default-to-idle for opt-in features.** Orphans with
   expensive runtime costs (`VCTSystem`, `SoftwareDenoiser`)
   should be initialised with `enabled = false` or a small
   footprint (32³ grid, disabled RT path) so the accessor is
   usable from frame 1 without burning CPU / memory.

6. **Test against the real class.** Phases L / O / P all found
   that audit-claimed "existing coverage" was actually against
   a local test-file reimplementation. **Grep the test file for
   the real class's fully-qualified name before trusting the
   claim.** Write new tests against the real symbols directly.

7. **Test quirks to remember:**
   - `EXPECT_EQ(enum_class_val, ...)` → cast both sides to
     `int` (test framework streams to `std::ostream`).
   - Batch-API tests should use non-multiple-of-4 counts to
     exercise both the unrolled loop and the scalar cleanup
     path.
   - Deterministic pseudo-random noise beats checkerboard
     patterns for signal-processing tests (Phase Q caught a
     variance-reduction test that passed with 0.12 = 0.12
     because the checkerboard defeated the bilateral filter).

8. **Lock in the build + test + format cycle:**
   ```bash
   cmake --preset linux-gcc-release
   cmake --build build/linux-gcc-release --target SparkEngine 2>&1 | grep -E "error:" | head
   cmake --build build/linux-gcc-release --target SparkTests 2>&1 | grep -E "error:" | head
   SPARK_TEST_NAME=<PhasePrefix> ./build/linux-gcc-release/bin/SparkTests
   ./build/linux-gcc-release/bin/SparkTests 2>&1 | grep "^Tests:"
   clang-format -i <touched files> && clang-format --dry-run --Werror <touched files>
   ```

9. **Commit + push — one phase per commit.** Knowledge entry
   (`.claude/knowledge/engine-next-steps-phase-<X>-2026-04-11.md`),
   `.claude/index.md` row update, and the code + tests land in
   a single commit. Message format matches Phases I→T:
   `feat(graphics): Phase <X> — activate <orphan> in <parent>`.

10. **Update this plan file.** When a phase lands, strike it
    off the "Recommended execution order" list and add a note
    pointing at the corresponding
    `engine-next-steps-phase-<X>-2026-04-11.md` entry.

---

## Files this plan touches (plan itself only)

```
.claude/knowledge/engine-next-steps-phase-u-plan-2026-04-11.md  (new — this file)
.claude/index.md                                                (row added for this plan)
```

No code, no tests — this is a planning document. Execution is
deferred to a future session.

---

## Context carryover for the future session

When resuming, the future session should:

1. Read `.claude/knowledge/engine-next-steps-phase-t-2026-04-11.md`
   for the most recent Tier 2 orphan activation state and the
   running totals across Phases I→T.
2. Read this plan document to pick a theme and a starting phase.
3. Run the `Session start` sequence from `CLAUDE.md`:
   ```bash
   git fetch origin Working
   git rebase origin/Working   # if behind
   cat .claude/index.md
   ```
4. Pick Phase U (`ShaderHotReload`) and follow the playbook
   section above.

The branch `claude/engine-roadmap-8Kc2c` will be in its Phase T
end state: clean working tree, 18 orphans activated, full suite
green on `linux-gcc-release`.
