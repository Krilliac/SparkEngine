# Golden Image Regression Tests

This directory holds reference ("golden") screenshots and the
reviewed-threshold manifest that `Utils/GoldenImageTest.h` uses to
detect visual regressions.

## Directory layout

```
Tests/GoldenImages/manifest.json          ← reviewed thresholds + baseline hashes (committed)
Tests/GoldenImages/<backendRow>/<scene>.png ← reference images (committed)
Tests/Output/                             ← run-time captures and diffs (not committed)
```

Backend rows are `d3d11-warp`, `d3d11-hw`, `opengl-llvmpipe` and
`vulkan-lavapipe`. A scene has one baseline per row it is certified on.

Committed baselines:

- `vulkan-lavapipe/`: `PostProcess_ACES`, `BloomExtract` and
  `GaussianBlur_Vertical`, the shipped SPIR-V post-process programs
  rendered on Mesa 25.2.8 Lavapipe by `Tests/TestRHI230VulkanGoldenReal.cpp`
  (RHI-230, CTest `VulkanGoldenTests`). They are software-row shader
  evidence, not engine-pass goldens or hardware certification.
- `opengl-llvmpipe/`: `LitSphere_BasicVS_BasicPS`, `PostProcess_ACES`,
  `PostProcess_Reinhard`, `PostProcess_Uncharted2`, `PostProcess_FXAA`,
  `GaussianBlur_Horizontal`, `GaussianBlur_Vertical` and `BloomExtract`,
  the shipped `Shaders/GLSL` programs rendered through `GLDevice` on
  Ubuntu 24.04's `noble-updates` Mesa `25.2.8-0ubuntu0.24.04.2` llvmpipe by
  `Tests/TestRHI240OpenGLGoldenReal.cpp` (RHI-240). Only CTest
  `SparkOpenGLGoldenTests` compares them (the main `SparkEngineTests` entry
  excludes `OpenGLGolden_`), and a hosted-runner match is not yet recorded.
  They are software-row shader evidence, not engine-pass goldens or
  hardware driver certification.

Both Linux rows are compared in `.github/workflows/build.yml` CI by the
Mesa-pinned `golden-linux` job. It is advisory (not a `required-ci-gate`
dependency) until its first hosted pass, and becomes required after that pass is
recorded. It fails first if the runner's `libgl1-mesa-dri` or
`mesa-vulkan-drivers` is not `25.2.8-0ubuntu0.24.04.2`, and then runs
`ctest -L '^(opengl-golden|vulkan-golden)$'`. Until that promotion,
`build-linux-gcc` and `build-linux-clang` also keep running both golden CTest
entries, so a required job still compares the baselines; promotion excludes them
there and makes `golden-linux` the single comparison. If Ubuntu moves Mesa,
re-render and re-review the baselines as described below.

The `d3d11-warp` and `d3d11-hw` rows have no entries, so their comparisons
fail closed. D3D11 baselines land with the RHI-210 golden slices, each
rendered on its real row and reviewed.

## Fail-closed rules

`GoldenImageTestRunner::CompareWithGolden` never skips. It returns
`matched == false` with a `failureReason` when:

- the configured `backendRow` is not one of the rows above, or the scene
  id is not 1-128 characters of `[A-Za-z0-9_-]`;
- `manifest.json` is missing, is not valid JSON, has the wrong
  `schemaVersion`, or has any invalid entry (the whole manifest is
  rejected, not just the entry);
- the manifest has no entry for the scene on the configured row;
- the baseline PNG is missing, or its SHA-256 differs from the reviewed
  `baselineSha256`;
- the baseline does not decode as a PNG (the pre-2026-09 raw-RGBA
  layout is rejected), no capture is set, or the capture size differs.

`RunAllComparisons` compares every manifest entry for the configured
row and returns a single failed result when there are none.

## Manifest schema

```json
{
  "schemaVersion": 1,
  "entries": [
    {
      "scene": "TriangleClear",
      "backendRow": "vulkan-lavapipe",
      "software": true,
      "perPixelThreshold": 10,
      "tolerancePercent": 0.5,
      "reviewer": "reviewer name",
      "baselineSha256": "<64 lowercase hex characters>"
    }
  ]
}
```

Every field is required and unknown keys are rejected, so a misspelled
threshold cannot silently fall back to a default.

- `software` must be `true` for `d3d11-warp`, `opengl-llvmpipe` and
  `vulkan-lavapipe`, and `false` for `d3d11-hw`.
- `perPixelThreshold` — Euclidean RGB distance (0-441.68) below which a
  pixel counts as matching.
- `tolerancePercent` — maximum percent (0-100) of differing pixels.
- `baselineSha256` — `sha256sum` of the committed baseline PNG.

Thresholds come only from the manifest; `GoldenImageConfig` has no
threshold fields. Different GPUs and drivers produce slightly
different floating-point output on the same render, which is why
thresholds are reviewed per row rather than shared.

## Image format

Baselines, captures and diffs are real 8-bit RGBA PNG files
(`Utils/GoldenImagePng.h`). They are encoded with the vendored miniz PNG
writer, the same one `Graphics/ScreenCapture.h` uses. The reader accepts only
8-bit RGB/RGBA non-interlaced PNGs with valid chunk CRCs and rejects
everything else, including the pre-2026-09 raw-RGBA layout.
`file Tests/GoldenImages/<row>/<scene>.png` reports `PNG image data`.

The bundled `ThirdParty/Utils/stb` headers are API stubs whose
`stbi_write_png` / `stbi_load` always fail, so they are not used here.

## Adding or updating a baseline

1. Render the scene on the target row and write the capture with
   `GoldenImageTestRunner::CaptureGolden("<scene>")` (writes
   `<goldenImageDir>/<backendRow>/<scene>.png`), or copy the actual frame
   the failed comparison left in `Tests/Output/<backendRow>_<scene>.png`.
2. Inspect the image. Choose thresholds and record them, your name as
   `reviewer`, and `sha256sum <row>/<scene>.png` as `baselineSha256` in
   `manifest.json`.
3. Commit the PNG and the manifest change together. The PR description
   must explain the visual change.

Until step 2 is done the hash check fails, so a new capture cannot pass
without review.

## Golden lanes and row parity

A golden lane is a `Tests/TestRHI*Golden*.cpp` source that declares
`constexpr const char* kRow` and a `kScenes` set. `Tests/Tools/test_golden_manifest.py`
(CTest `GoldenImage_ManifestIntegrity`) requires:

- every lane to be registered in `Tests/CMakeLists.txt` through
  `SPARK_TEST_FILE=<lane>;`;
- the union of `kScenes` over all lanes of one row to equal the manifest's
  scenes for that row, so no baseline goes uncompared and no lane compares a
  scene the manifest does not review;
- each scene of a row to be declared by exactly one lane of that row, so a
  baseline is compared once.

A row may therefore be split across several lane sources, one fixture per file
(for example a bare `PostProcessingPipeline`, a full `GraphicsEngine`, and the
canonical scene through `WorldBasicRenderer` on `d3d11-warp`).

## D3D11 WARP capture and review

`d3d11-warp` is a software row. Its rasterizer identity, the WARP analogue of
the Mesa pin on the Linux rows, is the file version of
`%SystemRoot%\System32\d3d10warp.dll` plus the Windows build number:

```powershell
(Get-Item "$env:SystemRoot\System32\d3d10warp.dll").VersionInfo.FileVersion
[System.Environment]::OSVersion.Version
```

The reviewer string of every `d3d11-warp` entry records both, for example
`Claude agent, RHI-210 (WARP d3d10warp.dll 10.0.26100.9278, Windows build 26200; ... owner review pending)`.
A baseline is added only after this checklist:

1. **Probes.** The lane's CPU-formula or geometry probes pass on the capture,
   so a broken render cannot become the baseline.
2. **Inspection.** The PNG is opened and looked at.
3. **Cross-row divergence.** The same scene is rendered on a local hardware
   device and the maximum and mean pixel distance to the WARP capture are
   recorded; they justify the chosen `perPixelThreshold` and
   `tolerancePercent`.
4. **Mutation.** Changing one constant in the shader under test makes the
   golden comparison fail with a pixel verdict (done locally, not committed).
5. **Pending marker.** The reviewer string keeps `owner review pending` until
   the owner reviews the baseline. That marker is allowed only on software
   rows; `d3d11-hw` baselines stay owner-gated.

WARP output may differ between Windows builds. A mismatch on another build
(for example the hosted `windows-2022` image) is data for owner review, never
a reason to loosen thresholds.

## Failure output

When a comparison fails on pixels, the runner writes to `outputDir`:

- `<backendRow>_<scene>.png` — the actual captured frame;
- `<backendRow>_<scene>_diff.png` — red = diverged (brighter = larger
  distance), dim green = matched.

Live-device RT tests on the macOS CI row upload `Tests/Output/` as an
artifact named `rt-goldens-<run-id>`.

## Blank-frame check

`GoldenImageTestRunner::AnalyzeFrame` and `FrameHasRenderedContent`
reject a uniform frame (fewer than two colours, or one colour covering
more than a given share). The D3D11 golden tests use them, and the
OpenGL and Vulkan golden tests should use the same functions rather
than a local copy.

## Metal captures

Metal-side capture uses
`Spark::RHI::Metal::ReadbackTextureRGBA8(mtlTexture, width, height)`
from `Graphics/RHI/Metal/MetalTextureReadback.h`, wrapped by
`MetalGoldenImageCapture` as an `IGoldenImageCapture`. Metal is not a
manifest backend row yet.
