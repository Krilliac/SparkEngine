# Game Packaging

The Game Packaging code provides a local staging pipeline that can copy engine output, game DLLs, and raw assets into a package directory. Its output is not a release artifact: `stable-v1` is blocked and has no same-commit package/install/run/signing/attestation certification. The current implementation supports target selectors, optional `.pdb` removal, and a size-only manifest; it does not cook asset formats, calculate checksums, or create SparkPak archives.

**Canonical source:** `SparkEngine/Source/Engine/Build/GamePackager.h` / `.cpp`
**Compatibility facade:** `SparkEngine/Source/Core/GamePackager.h` / `.cpp`

All packaging work is owned by the `Spark::Build::GamePackager` implementation;
the historical `Spark::GamePackager` Core API translates into that owner. The
canonical `PackageResult::filesCopied` count includes successfully copied
payload files (the executable, modules, assets, data, extras, and any retained
debug symbols) and excludes generated `manifest.txt` metadata. A failed legacy
package reports the payload files copied before failure, while its output path
remains empty and no manifest is published.

The behaviours this page describes, and every `spark_cli.py` command and option in
`Tools/spark-cli/README.md`, are mapped to the tests that prove them in
`Tools/spark-cli/claims.json`; the `CLI_ClaimsMatchBehavior` CTest fails when a
claim names a missing test or a documented surface has no claim.

## Installed qualification checks

The required Windows MSI consumer now requests interruption, plain repair and
two install/uninstall cycles. It compares reinstall payload hashes and retains
declared external user data. Its runtime helper runs installed asset integrity,
authored scene/material pixel checks, NullRHI save/reload and AppContainer
repository-denial controls. These paths still need a native exact-commit run;
they do not establish no-display execution or Windows sanitizer/soak evidence.
Native MSI commands use a verified non-elevated user token and per-user MSI
properties; WiX MSI generation requires CMake 3.29+ for `perUser` scope.
See the [Windows qualification commands and limitations](../../docs/readiness/WINDOWS-PACKAGE-QUALIFICATION.md).

`tools/check-module-asset-refs.py` validates the shared provenance policy schema,
the referenced rule's license, and its supporting evidence paths as well as
asset hashes. Missing or relabelled license metadata fails. `NOASSERTION`
remains truthful outside stable-v1; no license is inferred for excluded assets.

For a complete Linux build, `SPARK_ENABLE_PACKAGE_CONSUMER_TESTS=ON` registers
`PackageAssets_RepositoryUnreachable`. It installs and copies the runtime,
mounts only that copy and system runtime directories in bubblewrap, verifies
the source/build directories are absent, and audits strace file accesses.
Repository lookups, asset lookups outside the installed Assets root, and missing
successful asset opens fail. It requires bubblewrap, strace and user namespaces;
missing prerequisites fail rather than skip. This is Linux NullRHI evidence;
On native Windows FPS builds, `FPSPackage_RepositoryUnreachable` stages three
copies of the installed runtime. A fresh AppContainer runs the positive copy
on NullRHI and D3D11/WARP while source/build canaries must be unreadable; a
scene-less NullRHI copy and an asset-less D3D11 copy are negative controls.
The D3D11 run must audit a successful load of the package copy's `level1.scene`
and save a visible frame. The AppContainer writes to its own profile folder,
then the runner copies selected logs and the frame into the test output before
deleting that profile. This is a Windows-only local test; it does not establish
same-commit release evidence until the built package actually passes it.

`SPARK_ENABLE_PACKAGE_REPEATABILITY_TESTS=ON` registers
`PackageInstall_Repeatability` on either host. It uses the configured CPack tree's
real install rules, hashes every installed file, checks identical reinstalls,
then invokes `SparkUninstall.cmake` twice per cycle. Two cycles must leave only
the declared `UserData/profile.json` fixture with identical bytes and its parent
directory. This does not invoke or qualify native MSI/NSIS repair/uninstall.

The `D3D11PassGolden_*` tests cover post-processing passes. They do not supply a
canonical installed FPS scene baseline. Canonical package rendering remains
open until a reviewed WARP baseline and an installed-package run are available.

## Overview

> **Current readiness boundary:** the repository asset-integrity check is a
> blocking preflight, but `GamePackager` does not yet consume an immutable
> handle-based snapshot produced by that verification. A passing
> `Assets/assets.integrity.json` check must not be described as proof of the
> packaged input. RDY-020 remains open until the in-profile package smoke and
> verified-input handoff are implemented.
>
> The CMake/CPack stable-v1 runtime package installs only the SparkGameFPS
> runtime asset closure, with no `NOASSERTION` asset (OD-09), and ships a
> derived stable-v1 manifest; see
> [Asset Pipeline](Asset-Pipeline.md#stable-v1-package-asset-profile-od-09).
> `GamePackager` does not apply package profiles.
>
> **Binary dependencies (ENG-220).** Every MSVC image links the `/MD` CRT, so
> it imports `vcruntime140.dll`, `vcruntime140_1.dll` and `msvcp140.dll`,
> which a clean Windows machine does not have. The root `CMakeLists.txt`
> installs them app-local into `bin/` through `InstallRequiredSystemLibraries`,
> as the separate `redist` install component. It is separate because the
> BLD-100 symbol map (`tools/shipping_symbol_manifest.py`) stages
> `runtime`/`tools`/`samples` and requires a first-party PDB for every image,
> and Microsoft's DLLs have none in this build. CPack still packs `redist`
> into the ZIP and the MSI/NSIS installers (`cmake/SparkCPackOptions.cmake`).
> A manual stage must add it:
> `cmake --install <build> --config Release --component redist --prefix <stage>`.
>
> `tools/pe_import_closure.py <stage>` proves the stage's import closure. It
> parses every `*.exe`/`*.dll` import and delay-import table. Each imported
> DLL must sit beside its importer, be an API set (`api-ms-win-*`,
> `ext-ms-*`), or be an allowlisted OS DLL that is present in `System32`. The
> check never reads PATH, the build tree or the source tree, and a malformed
> image or an empty stage fails. `FPSPackage_InstalledRuntime` and
> `FPSHeadlessPackage_NullRHISaveReload` run it on the staged runtime, samples
> and redist components. `PEImportClosure_Contract` covers the checker with
> synthetic PE fixtures on every host. Debug stages are not checked, because
> the Debug CRT is not redistributable.
>
> **Open gap: Vulkan-enabled packages.** When the Vulkan SDK is found and
> `ENABLE_VULKAN` is ON (the `windows-release` default), `SparkEngine.exe` and
> `SparkServer.exe` hard-import `vulkan-1.dll`. Only a Vulkan GPU driver
> installs that DLL, so such a package does not start on a clean or GPU-less
> machine, NullRHI included, and the closure check correctly fails it. The
> closure is proven only for `ENABLE_VULKAN=OFF` packages, the
> `windows-shipping` profile. Configure an FPS package tree with
> `-DENABLE_VULKAN=OFF`, for example
> `cmake --preset windows-release -DSPARK_GAME_MODULES=SparkGameFPS -DENABLE_VULKAN=OFF -DBUILD_TESTS=ON`.
> `Tests/CMakeLists.txt` warns at configure time when an FPS tree links
> Vulkan. Closing the gap needs a delay-loaded, load-checked Vulkan backend.

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

The canonical build-facing result also exposes `filesCopied`, defined as the
number of successfully copied payload files; generated manifest metadata is
not counted. Debug packages count retained `.pdb` files, while release package
stripping removes them before publication.

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
cmake --build build/windows-release --config Release
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
