# Golden Image Regression Tests

This directory holds reference ("golden") screenshots used by
`Utils/GoldenImageTest.h` to detect visual regressions.

## Directory layout

```
Tests/GoldenImages/     ← reference images committed to git
Tests/Output/           ← run-time captures and diffs (not committed)
```

When a test compares against a missing reference, it records the
result as "no baseline" rather than failing. Use
`GoldenImageTestRunner::CaptureGolden("scene-name")` once to create
the baseline, then commit the generated `.png` file.

## Image format

Files use the raw-RGBA on-disk layout from
`Utils/GoldenImageTest.h::SavePNG` — a 4-byte little-endian `width`,
a 4-byte `height`, followed by `width * height * 4` bytes of RGBA
pixel data. The file extension is `.png` for tooling convenience
even though the contents are raw. A future sweep can swap the
encoder for a real PNG without touching the test API.

## Tolerances

Configurable on the `GoldenImageConfig` struct:

- `perPixelThreshold` — Euclidean channel distance below which a
  pixel counts as matching. Default 10 (0-255 range).
- `tolerancePercent` — maximum percent of differing pixels before the
  comparison fails. Default 0.5%.

Different GPUs and driver versions produce slightly different
floating-point output on the same render — these tolerances absorb
the per-vendor variation without hiding real regressions.

## Workflow for Metal RT tests

Live-device RT tests on the macOS CI row write captures to
`Tests/Output/`. When the comparison fails, the workflow uploads
the full `Tests/Output/` directory as an artifact named
`rt-goldens-<run-id>` so reviewers can download:

- `<scene>.png` — the actual captured frame (raw RGBA layout)
- `<scene>_diff.png` — red = diverged, dim green = matched

To update a baseline after an intentional rendering change:

1. Run the test locally, capture the new output.
2. Copy `Tests/Output/<scene>.png` over
   `Tests/GoldenImages/<scene>.png`.
3. Commit the reference image alongside the code change. PR
   description must explain the visual change.

## Adding a new RT scene reference

Metal-side capture uses
`Spark::RHI::Metal::ReadbackTextureRGBA8(mtlTexture, width, height)`
from `Graphics/RHI/Metal/MetalTextureReadback.h`. Plumb the returned
bytes into a custom `IGoldenImageCapture` implementation, or write
them directly via `GoldenImageTestRunner::SavePNG` in one-shot
capture utilities.

## Why raw RGBA, not real PNG

The framework is header-only to keep the test harness portable and
zero-dependency. A real PNG encoder would pull in `libpng` or
`stb_image_write`, both of which raise the complexity floor for
contributors trying to run a single test. If reference image size
starts mattering, revisit this trade-off — a 1920×1080 golden is
about 8 MB raw versus roughly 1 MB compressed.
