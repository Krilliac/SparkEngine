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

No baselines are committed yet: `manifest.json` has an empty `entries`
list, so every comparison currently fails closed. Baselines land with
the backend golden slices of RHI-210 (D3D11) and the OpenGL/Vulkan
golden work, each rendered on its real row and reviewed.

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
