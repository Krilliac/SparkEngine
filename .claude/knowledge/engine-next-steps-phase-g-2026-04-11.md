# Engine next-steps — Phase G (2026-04-11)

**Status:** Active. Closes the four deferred items Phase F left open.
Two of them had to be deferred because they require Windows hardware
— those are documented here as reproducible procedures for any future
Windows session, not as work that can happen from Linux CI.

## Items closed

### G1 — Per-species billboard aspect ratio

**Problem:** Phase E hardcoded `FOLIAGE_IMPOSTOR_SCALE = 2.0` in
`FoliageVS.hlsl`. Phase F moved height into `FoliageSpecies` and the
cell meta buffer but still derived horizontal half-width as
`height * 0.5`. Grass clumps (wider than tall) and palm fronds (same)
can't use the one-ratio-fits-all approach.

**Fix (3 files, ~20 lines):**

- `FoliageSystem.h` — add `float billboardAspect = 0.5f;` to
  `FoliageSpecies`. Default keeps the Phase E/F appearance: half-width
  = height × 0.5 = height / 2.

- `FoliageImpostorBaker.cpp::UploadCellBuffer` — read
  `species->billboardAspect` with the same 0.01 min clamp pattern as
  `billboardHeight`, and pack it into the meta float4's `.y` slot:
  `meta = (billboardHeight, billboardAspect, 0, 0)`.

- `FoliageVS.hlsl` impostor branch — replace the `billboardHeight * 0.5`
  literal with `billboardHeight * billboardAspect` read from
  `meta.y`. The mesh sub-pass is untouched.

**Tests (CPU-only, in `Tests/TestFoliageRenderer.cpp`):**

- `FoliageRenderer_SpeciesDefaultsForPhaseFFields` extended to assert
  `billboardAspect == 0.5f`.
- `FoliageRenderer_BillboardHalfWidthFormulaPhaseG` documents the
  `halfWidth = height * aspect` formula and pins two anchor cases
  (tree: 8 × 0.5 = 4, bush: 1 × 1.5 = 1.5) so future changes to either
  the CPU-side packing or the VS read break a test before they break
  a pixel.

### G2 — Offline FXC validation for the foliage shader family

**Problem:** Phase E/F's foliage VS/PS and Phase C's foliage impostor
bake VS/PS are compiled at runtime via `D3DCompileFromFile`. The
runtime path is correct and shipped, but shader errors don't surface
until the first time a foliage scene is loaded — which in CI never
happens. A typo in the shaders could live on `Working` for weeks.

**Fix (CMakeLists.txt, ~60 lines):** mirror Phase D's DXC block with
a parallel FXC block. Gated on `WIN32 AND NOT MINGW` and
`find_program(FXC_EXECUTABLE ...)` succeeding. When fxc is found,
registers a `POST_BUILD` custom command per shader target that runs
`fxc /T <profile> /E <entry> /Fo <output.cso> <input.hlsl>` against:

| Shader | Entry | Profile |
|---|---|---|
| `FoliageVS.hlsl` | `main` | `vs_5_0` |
| `FoliagePS.hlsl` | `main` | `ps_5_0` |
| `FoliageImpostorBake.hlsl` | `VSMain` | `vs_5_0` |
| `FoliageImpostorBake.hlsl` | `PSMain` | `ps_5_0` |

The `.cso` outputs land next to the runtime `.hlsl` copies under
`$<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL/` so a future session
can switch the runtime to offline-load with a one-line change to
`FoliageRenderer::CompileFoliageShader` (and the equivalent in
`FoliageImpostorBaker::CompileBakeShaders`) — just replace
`D3DCompileFromFile` with a `ReadFile` + `CreateVertexShader/
CreatePixelShader` pair, mirroring `DXRManager::LoadDXILBlob`.

**Non-fatal when fxc is missing.** Prints a `STATUS` message with
install hints and the build succeeds — exactly the same pattern the
DXC block uses.

**Linux / MinGW builds are unaffected.** The entire block is wrapped
in `if(WIN32 AND NOT MINGW)`.

### G3 — Windows smoke test procedure (documented, requires Windows)

**Why deferred:** This session's environment is Linux with no GPU.
Running SparkEngine under a D3D11-capable Windows session with a
foliage scene is the only way to observe that Phase E's draw pass
actually puts pixels on screen. The work cannot happen from CI.

**Procedure for any future Windows session:**

1. **Build** — standard preset:
   ```
   cmake --preset windows-release
   cmake --build build --config Release
   ```
   If FXC is installed (G2 active), any shader error surfaces here
   before the runtime is touched.

2. **Open a scene with scattered foliage.** Either:
   - `Assets/Scenes/foliage-smoke.scene` if one exists, or
   - Any level and drop a `FoliageVolume` via the editor's Foliage
     panel with 1–3 species pointing at meshes that actually ship
     (`Assets/Models/*.obj`) and verify the FoliageManager registry
     has `albedoTexturePath` pointing at a real texture.

3. **Observe:**
   - **Near the volume** (closer than `FoliageRenderer::GetImpostorDistance()`,
     default 50 m): mesh instances draw with wind sway. The albedo
     cache pulls the species' texture via `TextureSystem::LoadTexture`
     on the first draw; a flat-green fallback means the cache hit the
     missing-file branch — check the log for
     `"FoliageRenderer: albedo load failed for '<path>' — using white fallback"`.
   - **Far from the volume:** impostor billboards face the camera,
     sampled from the atlas via `ImpostorCells[materialId*2+0]`. The
     billboard size matches `species->billboardHeight` and the aspect
     matches `species->billboardAspect` from Phase G.

4. **D3D11 debug layer checks.** Launch with the debug layer enabled
   and watch for:
   - `"The Shader Resource View in slot X of the Pixel Shader unit is
     going to be used..."` — normally harmless but indicates an SRV
     we bound but never unbound. `RenderFoliagePass` already clears
     t0..t3 on exit.
   - `"IASetVertexBuffers: Index X uses Stride Y which is not a
     multiple of 4..."` — indicates the 100-byte mesh vertex vs the
     44-byte unit quad got crossed. The input layout uses
     TEXCOORD0@offset 36 specifically to cover both strides; any
     "Vertex Buffer not long enough" warning here means a species
     mesh is using a vertex format other than `MeshAssetData::Vertex`.

5. **Matrix-convention verification.** The existing mesh path uses
   `mul(input.Pos, inst.worldMatrix)` under HLSL column-major default.
   `FoliageRenderer::BuildWorldMatrix` writes row-major. If the first
   Windows run shows foliage meshes transposed (rotated by 90° around
   an unexpected axis), transpose `inst.worldMatrix` in the VS before
   the `mul` — this is noted in the Phase E plan as the single place
   convention mismatch could surface.

6. **Log the results** — update this file's "Windows smoke run
   <date>" subsection below with pass/fail + any observations so the
   next session doesn't re-walk the same ground.

**Windows smoke run history:**

_(none yet — this file is documentation only until a Windows host
runs the procedure)_

### G4 — DXR Windows runtime tests (blocker documented)

**Why deferred:** Phase D added platform-agnostic tests for the parts
of `DXRManager` that don't depend on a real device
(`Tests/TestDXRSupport.cpp`, 13 tests / 47 assertions), but the actual
D3D12 + DXR code paths require:

1. A Windows host (CI has one: `build-windows-vs2022` on
   `windows-latest`).
2. A DXR-capable GPU — `windows-latest` runners do not have one.
3. Pre-compiled `.cso` blobs from Phase D's DXC step — present on any
   host with `dxc.exe` in the SDK, but the GitHub `windows-latest`
   runner doesn't bundle one by default.

**Options for a future session:**

- **Option A — dxvk / vkd3d + Lavapipe under Wine.** Spark already
  has the MinGW + Wine cross-compile path for D3D11
  (`.claude/knowledge/mingw-wine-cross-compilation.md`). vkd3d-proton
  advertises partial DXR 1.1 support on top of Vulkan raytracing.
  **Blocker:** Lavapipe (the Mesa software Vulkan driver used by
  the Linux CI runners) does not advertise the `VK_KHR_ray_tracing_*`
  extensions, so vkd3d's DXR path falls back to "not supported". This
  makes it unsuitable for CI.

- **Option B — D3D12 mock layer.** Implement a mock `ID3D12Device`
  + `ID3D12GraphicsCommandList` pair inside `Tests/` that records
  state transitions and argues via assertion that the right calls
  were made in the right order. Scope: roughly `TestDXRSupport.cpp`'s
  size again, maybe 300 lines. Covers the PSO build + SBT layout +
  dispatch counts paths that are currently untested. Does NOT cover
  real DXIL validation — that still needs dxc-compiled .cso files
  and a real device.

- **Option C — Windows CI runner with a DXR GPU.** Self-hosted GitHub
  runner with an RTX / Radeon RX 6xxx+ card. Real hardware coverage
  but requires infrastructure investment.

**Recommendation:** Pick Option B (D3D12 mock) when DXR becomes a
shipping-critical path. Until then, Phase D's platform-agnostic tests
are sufficient to catch enum/flag/null-device regressions, and any
real-device issues will surface during the Windows smoke test (G3)
because `DXRManager::Initialize` is called from the same engine
startup path.

## Files touched

```
SparkEngine/Source/Graphics/FoliageSystem.h              (G1)
SparkEngine/Source/Graphics/FoliageImpostorBaker.cpp     (G1)
Shaders/HLSL/FoliageVS.hlsl                              (G1)
Tests/TestFoliageRenderer.cpp                            (G1) + extended
CMakeLists.txt                                           (G2)
.claude/knowledge/engine-next-steps-phase-g-2026-04-11.md (new) (G3, G4)
.claude/index.md                                         (updated)
```

## Build status

- `cmake --preset linux-gcc-release` — clean.
- `cmake --build build/linux-gcc-release --config Release` — clean.
- `ctest` — **4353 passed, 0 failed, 1 warned** (pre-existing
  LoadTest flaky), total 118530+ assertions.
- `clang-format` — clean on all changed code files.
- The new FXC CMake block is a no-op on Linux (gated on
  `WIN32 AND NOT MINGW`) so the Linux build is byte-identical to
  Phase F except for the tiny code changes.

## What's left for Phase H

Phase G closes every Phase E/F deferral that can be closed from
Linux. What remains is work that either (a) requires Windows
hardware (G3's smoke test, G4's real DXR tests) and must wait until
a Windows session runs, or (b) is bigger in scope than the phased
work has been tackling:

- Foliage species albedo → normal / ORM maps for PBR-quality mesh
  foliage. Would need matching CB additions and a PBR shading path.
- Wind-phase deterministic GPU replay (the Phase E wind path is
  fine for visualization but not deterministic across replays).
- The top-level `Shaders/HLSL/` tree has ~91 files. G2 only
  compiles the 4 foliage-family shaders. A unified compile step
  for every shader in the tree is its own engineering session —
  each subsystem owns its own profile + entry point combination.
