# Golden Image Testing

The Golden Image Testing framework captures framebuffer screenshots, compares them pixel-by-pixel against stored reference images, and reports visual regressions. It supports configurable per-pixel and per-image tolerance thresholds, generates diff images highlighting changed regions, and integrates with CI pipelines for automated visual regression detection.

**Source:** `SparkEngine/Source/Utils/GoldenImageTest.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `GoldenImageTestRunner` | Singleton that captures screenshots, compares against golden references, and manages the test workflow |
| `IGoldenImageCapture` | Abstract interface for framebuffer readback (one implementation per RHI backend) |
| `ImageComparisonResult` | Detailed result of comparing a captured screenshot against a golden image |
| `GoldenImageConfig` | Configuration for directories, tolerance thresholds, and reporting limits |
| `PixelDiff` | Information about a single pixel that differs between golden and actual images |

## Key Types

### GoldenImageConfig

Configuration struct controlling directories and tolerance:

```cpp
struct GoldenImageConfig
{
    std::string goldenImageDir;      // Directory containing reference images
    std::string outputDir;           // Directory for captured / diff images
    float tolerancePercent = 0.5f;   // Max allowed percent of differing pixels
    float perPixelThreshold = 10.0f; // Max channel distance before a pixel counts as different
    uint32_t maxDiffsToReport = 100; // Cap on PixelDiff entries stored in results
};
```

### ImageComparisonResult

Full result of a golden image comparison:

```cpp
struct ImageComparisonResult
{
    std::string sceneName;            // Scene/test name
    bool matched = true;              // True if within tolerance
    uint32_t totalPixels = 0;         // Total pixel count (width * height)
    uint32_t differentPixels = 0;     // Number of pixels exceeding threshold
    float percentDifferent = 0.f;     // Percentage of differing pixels
    float maxPixelDistance = 0.f;     // Maximum per-pixel distance observed
    float averagePixelDistance = 0.f; // Average distance across all pixels
    std::string diffImagePath;        // Path to the generated diff image
    std::vector<PixelDiff> diffs;     // First N differing pixels (capped)
};
```

### PixelDiff

Details about a single mismatched pixel:

```cpp
struct PixelDiff
{
    uint32_t x = 0;        // Pixel X coordinate
    uint32_t y = 0;        // Pixel Y coordinate
    uint8_t expectedR = 0; // Golden red channel
    uint8_t expectedG = 0; // Golden green channel
    uint8_t expectedB = 0; // Golden blue channel
    uint8_t actualR = 0;   // Actual red channel
    uint8_t actualG = 0;   // Actual green channel
    uint8_t actualB = 0;   // Actual blue channel
    float distance = 0.f;  // Euclidean distance in RGB space
};
```

### IGoldenImageCapture

Abstract interface for capturing the current framebuffer. Implement this per RHI backend:

```cpp
class IGoldenImageCapture
{
public:
    virtual ~IGoldenImageCapture() = default;

    // Capture the current framebuffer as RGBA pixels
    // Returns RGBA byte vector of size width * height * 4
    virtual std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) = 0;
};
```

## Quick Start

### Setting Up the Test Runner

```cpp
#include "Utils/GoldenImageTest.h"

auto& runner = Spark::GoldenImageTestRunner::GetInstance();

// Configure directories and tolerances
Spark::GoldenImageConfig config;
config.goldenImageDir = "Tests/GoldenImages";
config.outputDir = "Tests/Output";
config.tolerancePercent = 0.5f;    // Allow up to 0.5% of pixels to differ
config.perPixelThreshold = 10.0f;  // Per-pixel RGB distance threshold
config.maxDiffsToReport = 100;     // Report at most 100 differing pixels

runner.Initialize(config);
```

### Implementing a Capture Backend

Each RHI backend provides its own framebuffer readback. Here is an example for D3D11:

```cpp
class D3D11Capture : public Spark::IGoldenImageCapture
{
public:
    D3D11Capture(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_device(device), m_context(context) {}

    std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) override
    {
        // Create staging texture, copy from backbuffer, map and read RGBA data
        std::vector<uint8_t> pixels(width * height * 4);
        // ... D3D11 readback implementation ...
        return pixels;
    }

private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
};

// Inject the capture backend
runner.SetCapture(std::make_unique<D3D11Capture>(device, context));
```

### Capturing Golden Reference Images

On the first run (or whenever the expected visuals change), capture golden references:

```cpp
// Render the scene, then capture
renderEngine.RenderFrame();
runner.CaptureGolden("MainMenu");

renderEngine.LoadScene("Level1");
renderEngine.RenderFrame();
runner.CaptureGolden("Level1_Spawn");
```

Golden images are saved to the `goldenImageDir` as `{sceneName}.png` files at a default resolution of 1920x1080.

### Comparing Against Golden Images

On subsequent runs, compare the current framebuffer against stored references:

```cpp
renderEngine.RenderFrame();
auto result = runner.CompareWithGolden("MainMenu");

if (!result.matched)
{
    std::println("Visual regression in {}: {:.2f}% pixels differ (max distance: {:.1f})",
                 result.sceneName, result.percentDifferent, result.maxPixelDistance);
    std::println("Diff image saved to: {}", result.diffImagePath);

    // Inspect individual pixel differences
    for (const auto& diff : result.diffs)
    {
        std::println("  Pixel ({}, {}): expected RGB({},{},{}), got RGB({},{},{}) distance={:.1f}",
                     diff.x, diff.y,
                     diff.expectedR, diff.expectedG, diff.expectedB,
                     diff.actualR, diff.actualG, diff.actualB,
                     diff.distance);
    }
}
```

### Running All Comparisons

Compare every golden image in the golden directory at once:

```cpp
auto results = runner.RunAllComparisons();

if (Spark::GoldenImageTestRunner::HasRegressions(results))
{
    for (const auto& r : results)
    {
        if (!r.matched)
        {
            std::println("FAIL: {} -- {:.2f}% different", r.sceneName, r.percentDifferent);
        }
    }
}
else
{
    std::println("All {} golden image tests passed.", results.size());
}
```

### Updating Golden Images

When visuals change intentionally, update the golden references:

```cpp
runner.UpdateGolden("MainMenu");  // Equivalent to CaptureGolden
```

### Listing Available Golden Images

```cpp
auto names = runner.GetGoldenImageNames();
for (const auto& name : names)
{
    std::println("Golden image: {}", name);
}
```

## Configuration

### Tolerance Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `tolerancePercent` | 0.5% | Maximum percentage of pixels allowed to differ before the test fails |
| `perPixelThreshold` | 10.0 | Per-pixel Euclidean RGB distance threshold; pixels below this are considered matching |
| `maxDiffsToReport` | 100 | Maximum number of `PixelDiff` entries stored in results |

### Per-Pixel Distance Calculation

Pixel distance is computed as the Euclidean distance in RGB space (alpha is ignored):

```
distance = sqrt((Rg - Ra)^2 + (Gg - Ga)^2 + (Bg - Ba)^2)
```

Where `g` = golden and `a` = actual. The maximum possible distance is approximately 441.7 (`sqrt(3 * 255^2)`).

A pixel is counted as "different" only if its distance exceeds `perPixelThreshold`. The test passes only if the percentage of different pixels is at or below `tolerancePercent`.

### Diff Image Generation

When a comparison fails, a diff image is generated showing:

- **Red pixels**: Differing regions, with intensity proportional to the distance (brighter red = larger difference)
- **Dark green pixels**: Matching regions

The diff image is saved to `{outputDir}/{sceneName}_diff.png`.

## Console Commands

```cpp
std::string status = runner.Console_GetStatus();
// Output: "[GoldenImageTest] goldenDir=Tests/GoldenImages, outputDir=Tests/Output,
//          tolerance=0.5%, perPixel=10, capture=set"
```

## CI Integration

### Automated Visual Regression Testing

```cpp
int main()
{
    auto& runner = Spark::GoldenImageTestRunner::GetInstance();

    Spark::GoldenImageConfig config;
    config.goldenImageDir = "Tests/GoldenImages";
    config.outputDir = "Tests/CIOutput";
    config.tolerancePercent = 1.0f;   // Slightly relaxed for CI
    config.perPixelThreshold = 15.0f;
    runner.Initialize(config);
    runner.SetCapture(std::make_unique<SoftwareCapture>());

    // Render test scenes and compare
    for (const auto& scene : testScenes)
    {
        RenderScene(scene);
        auto result = runner.CompareWithGolden(scene);
        if (!result.matched)
        {
            std::print(stderr, "VISUAL REGRESSION: {} ({:.2f}% diff)\n",
                       result.sceneName, result.percentDifferent);
        }
    }

    auto results = runner.RunAllComparisons();
    return Spark::GoldenImageTestRunner::HasRegressions(results) ? 1 : 0;
}
```

### Software Rendering in CI

For GPU-less CI environments, use NullRHIDevice or Mesa llvmpipe for software rendering. The capture interface works with any RHI backend.

## Integration

### With the RHI

Each RHI backend (D3D11, D3D12, Vulkan, OpenGL, Metal) can provide its own `IGoldenImageCapture` implementation for framebuffer readback. The `NullRHIDevice` can return blank frames for headless testing.

### With the Benchmark Framework

Combine visual and performance regression testing:

```cpp
// Run performance benchmarks
auto& bench = Spark::BenchmarkFramework::GetInstance();
auto perfResults = bench.RunAll();
auto perfComparisons = bench.CompareWithBaseline(perfResults, baselines);

// Run visual regression tests
auto& runner = Spark::GoldenImageTestRunner::GetInstance();
auto vizResults = runner.RunAllComparisons();

bool allPassed = !bench.HasRegressions(perfComparisons)
              && !Spark::GoldenImageTestRunner::HasRegressions(vizResults);
```

### Static Comparison Utility

The `CompareImages` static method can compare any two RGBA buffers without the full runner:

```cpp
auto result = Spark::GoldenImageTestRunner::CompareImages(
    goldenData, actualData, width, height, perPixelThreshold);

std::println("Diff: {} pixels ({:.2f}%), max distance: {:.1f}",
             result.differentPixels, result.percentDifferent,
             result.maxPixelDistance);
```

### Static PNG I/O

Save and load raw RGBA images (simplified format: `[width:4][height:4][RGBA data]`):

```cpp
// Save
Spark::GoldenImageTestRunner::SavePNG("output.png", pixelData, width, height);

// Load
uint32_t w, h;
auto pixels = Spark::GoldenImageTestRunner::LoadPNG("input.png", w, h);
```

## API Reference

### GoldenImageTestRunner (Singleton)

| Method | Description |
|--------|-------------|
| `GetInstance() -> GoldenImageTestRunner&` | Access the singleton |
| `Initialize(const GoldenImageConfig&)` | Set up directories and tolerances |
| `Shutdown()` | Release capture interface and reset config |
| `SetCapture(unique_ptr<IGoldenImageCapture>)` | Set the framebuffer capture backend |
| `CaptureGolden(string_view sceneName)` | Capture and save a golden reference (1920x1080) |
| `CompareWithGolden(string_view) -> ImageComparisonResult` | Compare current framebuffer against stored golden |
| `RunAllComparisons() -> vector<ImageComparisonResult>` | Compare all golden images in the directory |
| `HasRegressions(vector<ImageComparisonResult>) -> bool` | Check if any results did not match (static) |
| `UpdateGolden(string_view sceneName)` | Overwrite golden reference with current frame |
| `GetGoldenImageNames() -> vector<string>` | List all golden image scene names |
| `CompareImages(golden, actual, w, h, tolerance) -> ImageComparisonResult` | Static pixel-by-pixel comparison |
| `SavePNG(string_view, data, w, h) -> bool` | Save RGBA data to file (static) |
| `LoadPNG(string_view, w&, h&) -> vector<uint8_t>` | Load RGBA data from file (static) |
| `Console_GetStatus() -> string` | Human-readable status for console |

### IGoldenImageCapture (Interface)

| Method | Description |
|--------|-------------|
| `CaptureFramebuffer(uint32_t w, uint32_t h) -> vector<uint8_t>` | Capture framebuffer as RGBA pixels |

## Thread Safety

- `GoldenImageTestRunner` is a singleton with **no internal synchronization**. All methods must be called from the **main thread** (or a single test thread).
- `CompareImages`, `SavePNG`, and `LoadPNG` are static methods with no shared state and are safe to call from any thread.
- `GetInstance()` uses a function-local static and is safe for concurrent first-access under C++11 magic-statics guarantees.
- The `IGoldenImageCapture` implementation may interact with the GPU; ensure framebuffer readback happens after the frame is fully rendered.

## See Also

- [[Benchmark-Framework]] -- Performance regression testing
- [[Graphics-Engine]] -- Rendering pipeline and framebuffer management
- [[RHI-Overview]] -- RHI abstraction layer and backend implementations
- [[Testing]] -- Unit test infrastructure and CTest setup
