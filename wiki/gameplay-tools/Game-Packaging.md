# Game Packaging

The Game Packaging code provides a local staging pipeline that can copy engine output, game DLLs, and raw assets into a package directory. Its output is not a release artifact: `stable-v1` is blocked and has no same-commit package/install/run/signing/attestation certification. The current implementation supports target selectors, optional `.pdb` removal, and a size-only manifest; it does not cook asset formats, calculate checksums, or create SparkPak archives.

**Source:** `SparkEngine/Source/Core/GamePackager.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `GamePackager` | Singleton orchestrating local staging: config validation, raw asset/binary copying, optional `.pdb` removal, and size-only manifest generation |
| `PackageConfig` | Configuration struct controlling output directory, platform, build config, and feature toggles |
| `PackageResult` | Result struct describing pipeline outcome: success flag, output path, file counts, errors, and warnings |

## Key Enums and Types

### TargetPlatform

Selects the target platform for the packaged build. The packager detects native platforms at initialization and adds cross-compilation targets automatically.

```cpp
enum class TargetPlatform : uint8_t
{
    Windows,
    Linux,
    macOS
};
```

### PackageBuildConfig

Controls whether the output is a debug or release build, which affects binary selection and symbol stripping behavior.

```cpp
enum class PackageBuildConfig : uint8_t
{
    Debug,
    Release
};
```

### PackageConfig

```cpp
struct PackageConfig
{
    std::string outputDir = "Build/Package"; // Root output directory
    std::string projectName = "SparkGame";   // Project name (used in folder/manifest)
    TargetPlatform platform = TargetPlatform::Windows;
    PackageBuildConfig buildConfig = PackageBuildConfig::Release;
    bool stripDebugSymbols = true;  // Remove .pdb files only
    bool compressAssets = true;     // Invokes the current inspection/counting hook; no archive is created
    bool includeEditor = false;     // Include editor binaries (rarely wanted)
};
```

### PackageResult

```cpp
struct PackageResult
{
    bool success = false;              // Overall success flag
    std::string outputPath;            // Absolute path to the packaged output
    float totalSizeMB = 0.0f;         // Total size of output in megabytes
    std::vector<std::string> errors;   // Fatal errors that prevented completion
    std::vector<std::string> warnings; // Non-fatal warnings
    uint32_t assetCount = 0;           // Number of assets included
    uint32_t dllCount = 0;             // Number of DLLs/shared libraries copied
};
```

## Quick Start

### Minimal packaging example

```cpp
#include "Core/GamePackager.h"

void PackageMyGame()
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    Spark::PackageConfig cfg;
    cfg.outputDir    = "Build/Package";
    cfg.projectName  = "MyGame";
    cfg.platform     = Spark::TargetPlatform::Windows;
    cfg.buildConfig  = Spark::PackageBuildConfig::Release;
    cfg.compressAssets = true; // Current hook validates/counts only; it does not create SparkPak output

    auto result = packager.Package(cfg);
    if (!result.success)
    {
        for (const auto& err : result.errors)
            Log::Error("Packaging", err);
    }
    else
    {
        Log::Info("Packaging", "Output: {} ({:.1f} MB, {} assets, {} DLLs)",
                  result.outputPath, result.totalSizeMB,
                  result.assetCount, result.dllCount);
    }
}
```

### Validating config before packaging

You can dry-run a configuration check without executing the pipeline:

```cpp
auto errors = packager.ValidateConfig(cfg);
if (!errors.empty())
{
    for (const auto& e : errors)
        Log::Error("PackageConfig", e);
    return; // Do not proceed
}
```

Validation catches: empty output directory, empty project name, invalid filesystem characters in the project name, and uninitialized packager state.

### Packaging for Linux from a Windows host

Cross-platform target selectors are available because the packager copies files regardless of host platform. Linux selection is experimental and outside `stable-v1`; a selected target does not prove a runnable package:

```cpp
Spark::PackageConfig cfg;
cfg.platform    = Spark::TargetPlatform::Linux;
cfg.projectName = "MyGame";
cfg.buildConfig = Spark::PackageBuildConfig::Release;
cfg.stripDebugSymbols = false; // .pdb stripping is Windows-specific

auto result = packager.Package(cfg);
```

### Debug build with editor binaries

```cpp
Spark::PackageConfig cfg;
cfg.buildConfig       = Spark::PackageBuildConfig::Debug;
cfg.stripDebugSymbols = false;   // Keep .pdb files for debugging
cfg.includeEditor     = true;    // Include SparkEditor binaries
cfg.compressAssets    = false;   // Disable the current inspection/counting hook

auto result = packager.Package(cfg);
// Debug packages include .pdb files alongside DLLs
```

## Configuration

### PackageConfig defaults

| Field | Default | Notes |
|-------|---------|-------|
| `outputDir` | `"Build/Package"` | Root output directory, created automatically |
| `projectName` | `"SparkGame"` | Used in output folder name and manifest header |
| `platform` | `Windows` | Target platform for binary selection |
| `buildConfig` | `Release` | Controls binary source path and symbol stripping |
| `stripDebugSymbols` | `true` | Only applies when `buildConfig == Release` |
| `compressAssets` | `true` | Invokes a validation/counting hook; no `.spk` archive is currently created |
| `includeEditor` | `false` | Excludes editor binaries and editor-only assets |

### Output directory structure

The packager creates the following layout under the output root:

```
Build/Package/MyGame_Windows_Release/
    Bin/                    -- Engine executable + game DLLs
        SparkEngine.exe
        SparkGame.dll
        SparkGameFPS.dll
    Assets/                 -- Raw copied assets; no .spk archive is currently produced
        Textures/
        Models/
        Shaders/
    Config/                 -- Configuration files
    manifest.txt            -- File listing with sizes only; no checksums
```

The folder name follows the pattern `{projectName}_{platform}_{config}`.

At runtime the packaged game does not write inside this tree. `Saves/`, `Logs/`
(rotating `SparkEngine_<timestamp>.log`), `spark_trace.json`, and `ShaderCache/`
live under `%LOCALAPPDATA%/SparkEngine` (POSIX: `$XDG_DATA_HOME` or
`~/.local/share/SparkEngine`), and `settings.ini` resolves to
`%LOCALAPPDATA%/SparkEngine/Config/settings.ini` (`$XDG_CONFIG_HOME` or
`~/.config/SparkEngine`) whenever a user copy exists or the install's `Config/` is
not writable. `Data/*.spk` is resolved beside the executable first. Engine shaders
are no longer flattened on install: `Shaders/ForwardPlus/*.hlsl` ships as a
subdirectory and no prebuilt `Basic*.cso` is shipped.

## Console Commands

The packager exposes a console status command:

| Command | Description |
|---------|-------------|
| `Console_GetStatus()` | Returns initialization state, supported platform count, and last package result details |

Example output from `Console_GetStatus()`:

```
GamePackager: initialized, 3 supported platform(s)
  Last package: /abs/path/Build/Package/MyGame_Windows_Release (success)
  Assets: 247, DLLs: 5, Size: 142.3 MB
```

## Packaging Pipeline

The `Package()` method executes these steps in order:

1. **Validate configuration** -- Checks for empty fields, invalid characters, initialization state
2. **Copy assets** -- `CookAssets()` recursively copies raw `Assets/` files, skipping `Editor/` assets unless `includeEditor` is set; it performs no format cooking
3. **Copy binaries** -- Copies `.dll`/`.so`/`.dylib` and `.exe` files from `build/{Config}/` to `Bin/`; skips editor binaries unless requested
4. **Strip debug symbols** -- In Release mode with `stripDebugSymbols`, removes `.pdb` files from the output `Bin/` directory
5. **Generate manifest** -- Writes `manifest.txt` with project metadata, timestamp, and a listing of all files with sizes
6. **Inspect compression input** -- When `compressAssets` is enabled, `CompressOutput()` counts regular files and may emit warnings; it currently writes no archive
7. **Calculate total size** -- Walks the output tree and sums file sizes for the result

If any step produces fatal errors, the pipeline returns early with `success = false`.

## End-to-End Walkthrough

### Step 1: Build your game

```bash
cmake --preset windows-release
cmake --build build --config Release
```

### Step 2: Package from C++

```cpp
auto& packager = Spark::GamePackager::GetInstance();
packager.Initialize();

Spark::PackageConfig cfg;
cfg.outputDir   = "Dist";
cfg.projectName = "MyShooter";
cfg.platform    = Spark::TargetPlatform::Windows;
cfg.compressAssets = true;
cfg.stripDebugSymbols = true;

auto result = packager.Package(cfg);

// Check warnings even on success
for (const auto& w : result.warnings)
    Log::Warn("Packaging", w);

if (result.success)
    Log::Info("Packaging", "Package generated for local validation (not release-certified): {}", result.outputPath);
```

### Step 3: Inspect the size-only manifest

The generated `manifest.txt` contains:

```
# SparkEngine Package Manifest
# Project: MyShooter
# Platform: Windows
# Config: Release
# Timestamp: 1743724800
# Assets: 312
# DLLs: 6

Bin/SparkEngine.exe 4521984
Bin/SparkGame.dll 1048576
Assets/Textures/player.dds 2097152
...
```

### Step 4: Distribute

The output may be used for local validation. Do not distribute it as a certified release until the same-commit packaging, signing, attestation, install, upgrade, and rollback gates pass.

## Integration

### With AssetValidator

Run asset validation before packaging to catch broken references:

```cpp
auto& validator = Spark::AssetValidator::GetInstance();
auto report = validator.ValidateAll();
if (report.failCount > 0)
{
    Log::Error("Package", "Fix {} asset errors before packaging", report.failCount);
    return;
}
// Proceed with packaging
```

### With EngineContext

Register the packager at engine startup:

```cpp
auto& packager = Spark::GamePackager::GetInstance();
packager.Initialize();
// Packager is available via GetInstance() throughout the engine lifetime
```

### With Game Modules

Game module DLLs (SparkGame, SparkGameFPS, etc.) are automatically discovered in the build output directory and copied to the package `Bin/` folder. Editor module binaries are excluded unless `includeEditor` is set.

## API Reference

### GamePackager

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetInstance` | `static GamePackager& GetInstance()` | Get the singleton instance |
| `Initialize` | `void Initialize()` | Scan for available tools and supported platforms |
| `Shutdown` | `void Shutdown()` | Release resources |
| `Package` | `PackageResult Package(const PackageConfig& config)` | Execute the full packaging pipeline |
| `ValidateConfig` | `std::vector<std::string> ValidateConfig(const PackageConfig& config) const` | Validate config without executing |
| `GetSupportedPlatforms` | `std::vector<TargetPlatform> GetSupportedPlatforms() const` | List platforms this host can target |
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Human-readable status string |

### Private Helpers

| Method | Description |
|--------|-------------|
| `PlatformToString` | Convert `TargetPlatform` enum to display string |
| `GetDllExtension` | Get binary extension for target (`.dll`, `.so`, `.dylib`) |
| `GetExeExtension` | Get executable extension (`.exe` or empty) |
| `CookAssets` | Copy raw assets to output; no format cooking |
| `CopyBinaries` | Copy engine and game binaries to `Bin/` |
| `StripSymbols` | Remove `.pdb` files from output directory |
| `CreateManifest` | Write `manifest.txt` with file names and sizes; no checksums |
| `CompressOutput` | Count assets and emit warnings; no compression output |

## Thread Safety

`GamePackager` is **not thread-safe**. The `Package()` method performs extensive filesystem I/O and should be called from a single thread (typically the main thread or a dedicated packaging thread). Do not call `Package()` concurrently from multiple threads.

The singleton instance (`GetInstance()`) uses a function-local static and is safe to access from any thread after initialization, but all mutating operations must be serialized by the caller.

## See Also

- [Asset-Validation](Asset-Validation.md) -- Validate assets before packaging
- [Asset-Migration](Asset-Migration.md) -- Migrate asset formats between versions
- [ECS-Architecture](../subsystems/Entity-Component-System.md) -- Entity Component System overview
- [Build-System](../advanced/Build-System-and-CMake-Modules.md) -- CMake presets and build configuration
