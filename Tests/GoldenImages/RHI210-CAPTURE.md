# RHI-210 capture handoff

The three Primary WARP scenes are reviewed baselines, explicitly approved by
the user on 2026-10-02 in response to the exact hashed packet
`Sentinel_2378f2266ca48191873a1f638547f3b4`. Their manifest records zero RGB-distance
threshold, zero-percent tolerance, exact PNG hashes and Library image IDs.
Five normal captures were byte-identical. After registration the four-test
Primary suite passed, and each disabled pass produced an actual pixel mismatch
against its approved baseline. These are local WARP results, not release or
exact-commit hosted qualification.

The other seven scenes remain capture plans with `awaiting-capture` status and
thresholds to be set from measured variance. Existing post-processing entries
remain unchanged.

| Scene | Production path | Disabled-pass token |
| --- | --- | --- |
| `Frame_ForwardLit` | Forward geometry, depth and basic lighting | `forward` |
| `Frame_ForwardMaterial` | Forward material textures, normal map and specular | `forward-material` |
| `Frame_DrawList` | AssetPipeline and ECS draw list | `drawlist` |
| `Frame_PostChain` | Frame with GTAO, bloom, ACES and FXAA | `postchain` |
| `Scene_FPSLevel1_MainCamera` | Authored FPS arena through SceneManager and RenderScene | `scene` |
| `Scene_FPSLevel1_Overview` | Authored FPS arena from an elevated camera | `scene` |
| `World_OpaqueAndSprites` | WorldBasicRenderer opaque and alpha-blended sprite draws | `world` |
| `Primary_DeferredGeometry` | G-buffer albedo/encoded-normal/material captures and attachment write checks | `deferred-geometry` |
| `Primary_DeferredLighting` | LightingPass output before the later forward redraw | `deferred-lighting` |
| `Primary_ShadowDepth` | RenderShadowCasterDepth, the production shadow-pass callback | `shadow` |

The FPS fixture covers the authored scene, not the complete packaged gameplay
loop, weapon, HUD or time-dependent entities. Shadow capture checks the depth
pass itself, not sampled shadows in the final lit framebuffer. All new lanes
require the debug layer, including zero warning/error/corruption/discard counts.

## Production implementation and remaining qualification

The production deferred geometry pass now writes albedo, encoded normals and
material attachments, and the single-sample deferred resolve consumes those
attachments plus depth. Local WARP regressions passed for light, sky and material
changes/reversion, along with zero debug-layer diagnostics. The resolve remains
limited: no shadow visibility sampling, area lights, IBL or motion history;
point/spot behavior, fallback/recovery and complete scene lifecycle still need
their dedicated qualification. ShadowDepth validates caster depth output only.

RHI-210[0] remains incomplete: seven planned scenes and exact-commit hosted
evidence are still pending. The four CTest suite disabled flags remain unchanged
until their integration requirements are satisfied.

Integration on Windows (2026-10-01) found further production defects behind the
CPU-reference checks: the basic shader receives the plain inverse instead of the
inverse transpose for normals, Windows frustum culling builds its planes from
matrix rows instead of columns, the Windows texture loader decodes only TGA (a
PNG becomes the 2x2 fallback), and the reserved Plane primitive faces -y and is
culled from above. Until those fixes land, the four suites `D3D11FrameGolden`,
`D3D11SceneGolden`, `D3D11WorldGolden` and `D3D11PrimaryGolden` are registered
with the CTest `DISABLED` property, so `ctest` lists them as not run. Run a suite
directly while working on it, for example:

```powershell
$env:SPARK_TEST_FILE='TestRHI210D3D11FrameGoldenReal.cpp'
& build/windows-release/bin/Release/SparkTests.exe --warn-is-error
Remove-Item Env:SPARK_TEST_FILE
```

Remove the `DISABLED` property for a suite in the same change that lands its
fixes and its reviewed captures.

## Windows integration and capture

Run in the integrated checkout. The session sandbox cannot compile C++ or run
WARP. Install Windows Graphics Tools as the existing required
`build-windows-vs2022` job does, and use the pinned MSVC toolchain.

```powershell
powershell -NoProfile -File C:/Users/Nathan/.claude/scripts/fleet-preflight.ps1
powershell -NoProfile -File C:/Users/Nathan/.claude/scripts/build.ps1 'cmake --preset windows-release'
powershell -NoProfile -File C:/Users/Nathan/.claude/scripts/build.ps1 'cmake --build build/windows-release --config Release --target SparkTests SparkEngine --parallel 2'
ctest --test-dir build/windows-release -C Release -R '^(D3D11_Validation|D3D11_DeviceLoss|D3D11_Resource|D3D11PassGolden)$' --output-on-failure --no-tests=error
ctest --test-dir build/windows-release -C Release -R '^D3D11(Frame|Scene|World|Primary)Golden$' --output-on-failure --no-tests=error
```

The second CTest command is intentionally red while baselines are pending; it
must also expose the deferred implementation gaps. Capture from the binary's
runtime directory, with the staged assets produced by the SparkEngine build.
Find the built executable rather than guessing its configuration subdirectory:

```powershell
$rhi210Tests = @(Get-ChildItem -LiteralPath build/windows-release/bin -Recurse -Filter SparkTests.exe -File)
if ($rhi210Tests.Count -ne 1) { throw 'Select the exact Release SparkTests executable explicitly' }
python Tools/rhi210_capture.py --exe $rhi210Tests[0].FullName --output Tests/Output/rhi210-review
```

Use a fresh output directory for each capture attempt. Re-analyze a saved packet with
python Tools/rhi210_capture.py --analyze Tests/Output/rhi210-review. The tool retains five
runs, the disabled-pass runs, test logs and measurements. Captures remain failed
golden tests until reviewed baselines exist; successful file creation is not
test success. Unexpected assertions, missing captures, validation findings or
unchanged disabled-pass pixels must block the review packet.

For each golden, all pairs of the five runs determine the maximum Euclidean RGB
pixel variance. The candidate per-pixel threshold is its ceiling (zero for
identical captures), with zero differing-pixel tolerance. The disabled-pass
capture must exceed that candidate. These are measurements and proposals, not
approved threshold values. Inspect `measurements.json` and show the owner
`contact-sheet.html`, together with the WARP/OS identities:

```powershell
(Get-Item "$env:SystemRoot/System32/d3d10warp.dll").VersionInfo.FileVersion
[System.Environment]::OSVersion.Version
```

After explicit owner review, the integrator copies the chosen captures into
`Tests/GoldenImages/d3d11-warp/` and replaces each pending entry with the existing
reviewed schema: measured thresholds, reviewer record and exact PNG SHA-256.
Do not preserve the pending-only `status` or `thresholdPolicy` keys in a reviewed
entry. Preserve the measurements and review record in the change description.
Then run the normal goldens and prove an actual pixel-comparison failure for
every disabled-pass token against the reviewed baseline. For example:

```powershell
$env:SPARK_RHI210_DISABLE_PASS='forward'
ctest --test-dir build/windows-release -C Release -R '^D3D11FrameGolden$' --output-on-failure --no-tests=error
Remove-Item Env:SPARK_RHI210_DISABLE_PASS
ctest --test-dir build/windows-release -C Release -L d3d11-golden --output-on-failure --no-tests=error
ctest --test-dir build/windows-release -C Release --output-on-failure --no-tests=error
```

Repeat the disabled-pass command with the table's tokens and their matching
lane. The failure must include `matched=no` with a pixel verdict, not merely a
missing manifest/PNG, crash, validation warning or count mismatch.

## Cross-platform and metadata checks

From the integrator's WSL shell in the Linux checkout:

```bash
cmake --preset linux-gcc-release
cmake --build build/linux-gcc-release --target SparkTests --parallel 2
SPARK_TEST_FILE=TestGoldenImageTest.cpp build/linux-gcc-release/bin/SparkTests --warn-is-error --empty-is-error
ctest --test-dir build/linux-gcc-release --output-on-failure --no-tests=error
```

Windows-only test bodies are excluded on Linux; this checks the shared manifest
parser and integration, not D3D11 execution. Run these Python checks on either
host with its Python interpreter:

```text
python Tools/buildmatrix/inventory.py --output docs/readiness/build-matrix-inventory.json
python Tools/buildmatrix/inventory.py --check docs/readiness/build-matrix-inventory.json
python Tests/Tools/test_build_matrix_parity.py
python Tests/Tools/test_golden_manifest.py
python Tests/Tools/test_golden_review_gate.py
python Tests/Tools/test_rhi210_capture.py
python tools/site-data/validate.py
python tools/site-data/render_handoff.py
```

The actual required hosted producer is `build.yml/build-windows-vs2022`, Release
leg. The task restricts work-item edits to acceptance notes/evidence; its
`requiredCiJobs` therefore still names the planned `golden-d3d11` and
`d3d11-stress` jobs. The integrator must correct that declaration separately.
No criterion may be marked evidenced without a green exact-commit hosted run.
