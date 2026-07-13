# Asset Validation

The Asset Validation system provides a rule-based pipeline for verifying asset health. Built-in rules check texture references in materials, scene cross-references, shader compilability, and metadata completeness. Custom rules are added via the `IAssetValidationRule` interface.

`AssetValidator::Initialize()` is called during engine startup (in `GameplayLifecycleShared.cpp`, alongside the sibling diagnostic singletons), which registers the four built-in rules. Validation is then driven **on demand** via the `assetvalidate.*` console commands (see [Console Commands](#console-commands)); it is well-suited to a pre-packaging or editor-load pass, and [Game-Packaging](Game-Packaging.md) can invoke `ValidateDirectory()`/`ValidateAll()` directly.

**Source:** `SparkEngine/Source/Core/AssetValidator.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `AssetValidator` | Singleton validation pipeline that runs registered rules against assets; supports single-file, directory, and full-project scans |
| `IAssetValidationRule` | Abstract interface for implementing custom validation rules |
| `MaterialTextureValidator` | Built-in rule: checks that `.mat` files reference textures that exist on disk |
| `SceneReferenceValidator` | Built-in rule: validates `.scene`/`.scn` files for existence, size sanity, and broken references |
| `ShaderCompilationValidator` | Built-in rule: checks `.hlsl`/`.glsl`/`.vert`/`.frag`/`.comp` files for basic syntactic correctness |
| `AssetMetadataValidator` | Built-in rule: checks for zero-byte files, long filenames, and invalid path characters |
| `ValidationReport` | Aggregated report from a validation pass with counts and timing |
| `ValidationResult` | A single finding from one rule against one asset |

## Key Enums and Types

### ValidationSeverity

Each finding has a severity level that controls how it appears in reports and whether it blocks packaging:

```cpp
enum class ValidationSeverity : uint8_t
{
    Info,     // Informational (no action required)
    Warning,  // Potential issue that may cause runtime problems
    Error,    // Definite problem that will cause runtime failures
    Critical  // Blocking issue that prevents asset from being used
};
```

### ValidationResult

```cpp
struct ValidationResult
{
    ValidationSeverity severity = ValidationSeverity::Info;
    std::string assetPath;  // Path to the asset that triggered this finding
    std::string message;    // Human-readable description of the issue
    std::string suggestion; // Recommended fix (may be empty)
    uint32_t errorCode = 0; // Machine-readable error identifier
};
```

### ValidationReport

```cpp
struct ValidationReport
{
    std::vector<ValidationResult> results;
    uint32_t totalAssets = 0;  // Number of assets scanned
    uint32_t passCount = 0;    // Assets with no errors or warnings
    uint32_t failCount = 0;    // Assets with at least one Error or Critical
    uint32_t warningCount = 0; // Assets with only Warnings (no Error/Critical)
    std::string timestamp;     // ISO-8601 timestamp of the validation run
    float durationMs = 0.0f;   // Wall-clock duration in milliseconds
};
```

## Built-in Rules

### MaterialTextureValidator

Scans `.mat` files for texture path references. Checks that the material file exists and is non-empty, then looks for matching diffuse textures following the naming convention `<material>_diffuse.<ext>`.

| Error Code | Severity | Condition |
|-----------|----------|-----------|
| 1001 | Error | Material file does not exist |
| 1002 | Warning | Material file is empty (0 bytes) |
| 1003 | Info | No matching diffuse texture found |

Supported texture extensions: `.png`, `.jpg`, `.dds`, `.tga`, `.bmp`

### SceneReferenceValidator

Validates `.scene` and `.scn` files for existence and size sanity.

| Error Code | Severity | Condition |
|-----------|----------|-----------|
| 2001 | Error | Scene file does not exist |
| 2002 | Error | Scene file is empty (0 bytes) |
| 2003 | Warning | Scene file exceeds 100 MB |

### ShaderCompilationValidator

Checks shader source files for basic existence and size. This is a fast pre-check; full compilation validation uses the shader compiler.

| Error Code | Severity | Condition |
|-----------|----------|-----------|
| 3001 | Error | Shader source file does not exist |
| 3002 | Error | Shader source file is empty |
| 3003 | Warning | Shader file is suspiciously small (< 10 bytes) |

Supported extensions: `.hlsl`, `.glsl`, `.vert`, `.frag`, `.comp`

### AssetMetadataValidator

Validates general asset health: filename length, path characters, and zero-byte files.

| Error Code | Severity | Condition |
|-----------|----------|-----------|
| 4001 | Error | Asset file does not exist |
| 4002 | Warning | Filename exceeds 200 characters |
| 4003 | Warning | Path contains problematic characters (`#`, `%`, `&`, `{`, `}`) |
| 4004 | Warning | File is empty (0 bytes) |

## Quick Start

### Validate all assets in the project

```cpp
#include "Core/AssetValidator.h"

void CheckAllAssets()
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    auto report = validator.ValidateAll();

    Log::Info("Validation", "Scanned {} assets in {:.1f} ms",
              report.totalAssets, report.durationMs);
    Log::Info("Validation", "Pass: {}, Fail: {}, Warnings: {}",
              report.passCount, report.failCount, report.warningCount);

    for (const auto& r : report.results)
    {
        if (r.severity >= Spark::ValidationSeverity::Error)
            Log::Error("Validation", "[E{}] {}: {}", r.errorCode, r.assetPath, r.message);
        else if (r.severity == Spark::ValidationSeverity::Warning)
            Log::Warn("Validation", "[E{}] {}: {}", r.errorCode, r.assetPath, r.message);
    }
}
```

### Validate a single file

```cpp
auto report = validator.ValidateFile("Assets/Materials/brick.mat");
if (report.failCount > 0)
    Log::Error("Validation", "brick.mat has {} error(s)", report.failCount);
```

### Validate a subdirectory

```cpp
auto report = validator.ValidateDirectory("Assets/Levels");
// Only scans files under Assets/Levels/ recursively
```

### List registered rules

```cpp
auto rules = validator.GetRegisteredRules();
for (auto name : rules)
    Log::Info("Validation", "Rule: {}", name);
// Output:
//   Rule: MaterialTextureValidator
//   Rule: SceneReferenceValidator
//   Rule: ShaderCompilationValidator
//   Rule: AssetMetadataValidator
```

## Writing Custom Rules

Implement `IAssetValidationRule` to add project-specific checks:

```cpp
class TextureSizeValidator final : public Spark::IAssetValidationRule
{
public:
    void ValidateAsset(const std::filesystem::path& path,
                       Spark::ValidationReport& report) override
    {
        // Only check texture files
        auto ext = path.extension().string();
        if (ext != ".png" && ext != ".jpg" && ext != ".dds")
            return;

        namespace fs = std::filesystem;
        std::error_code ec;
        auto fileSize = fs::file_size(path, ec);
        if (ec)
            return;

        // Warn on textures larger than 16 MB
        if (fileSize > 16 * 1024 * 1024)
        {
            report.results.push_back({
                Spark::ValidationSeverity::Warning,
                path.string(),
                std::format("Texture is {:.1f} MB (recommended max: 16 MB)",
                           fileSize / (1024.0 * 1024.0)),
                "Reduce resolution or use compressed format (DDS/BC7)",
                5001
            });
        }
    }

    std::string_view GetRuleName() const override
    {
        return "TextureSizeValidator";
    }
};
```

Register it before running validation:

```cpp
auto& validator = Spark::AssetValidator::GetInstance();
validator.Initialize(); // Registers built-in rules

// Add custom rule
validator.RegisterRule(std::make_unique<TextureSizeValidator>());

// Now ValidateAll() includes your custom rule
auto report = validator.ValidateAll();
```

## CI Integration

Use the validator as a pre-packaging gate in your CI pipeline:

```cpp
auto& validator = Spark::AssetValidator::GetInstance();
validator.Initialize();

auto report = validator.ValidateAll();

// Fail CI if any Error or Critical findings exist
if (report.failCount > 0)
{
    Log::Error("CI", "Asset validation failed: {} errors", report.failCount);
    for (const auto& r : report.results)
    {
        if (r.severity >= Spark::ValidationSeverity::Error)
        {
            Log::Error("CI", "  [E{}] {}: {}", r.errorCode, r.assetPath, r.message);
            if (!r.suggestion.empty())
                Log::Error("CI", "    Fix: {}", r.suggestion);
        }
    }
    std::exit(1);
}
```

### Combining with packaging

```cpp
// Validate first, package only if clean
auto report = validator.ValidateAll();
if (report.failCount > 0)
{
    Log::Error("Package", "Cannot package: {} asset error(s)", report.failCount);
    return;
}

auto& packager = Spark::GamePackager::GetInstance();
packager.Initialize();
auto result = packager.Package(cfg);
```

## Console Commands

| Method | Description |
|--------|-------------|
| `Console_GetStatus()` | Returns initialization state, rule count, and last run summary |
| `Console_GetLastReport()` | Returns the full formatted validation report from the last run |

Example `Console_GetStatus()` output:

```
AssetValidator: initialized, 4 rule(s) registered
  Last run: 312 asset(s) scanned in 45.2 ms
  Pass: 298, Fail: 3, Warnings: 11
```

Example `Console_GetLastReport()` output:

```
=== Validation Report (2026-04-04T12:00:00Z) ===
Assets scanned: 312  |  Pass: 298  |  Fail: 3  |  Warnings: 11
Duration: 45.2 ms

[ERROR] Assets/Materials/missing.mat (E1001)
  Material file does not exist
  Suggestion: Verify the asset path
[WARN] Assets/Levels/huge_level.scene (E2003)
  Scene file is very large (142.3 MB)
  Suggestion: Consider splitting into streaming sub-scenes
```

## Integration

### With AssetMigration

Run migration before validation to ensure assets are in the current format:

```cpp
auto& migration = Spark::AssetMigrationRegistry::GetInstance();
// Migrate assets first...

auto& validator = Spark::AssetValidator::GetInstance();
auto report = validator.ValidateAll(); // Now validates current-version assets
```

### With GamePackager

The validator pairs naturally with the packaging pipeline as a pre-flight check. See the [Game-Packaging](Game-Packaging.md) wiki page.

### With SparkEditor

The editor can invoke `ValidateFile()` on individual assets at load time or `ValidateDirectory()` on the current project to show findings in the editor UI.

## API Reference

### AssetValidator

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetInstance` | `static AssetValidator& GetInstance()` | Get the singleton instance |
| `Initialize` | `void Initialize()` | Initialize with built-in rules |
| `Shutdown` | `void Shutdown()` | Release resources and clear rules |
| `RegisterRule` | `void RegisterRule(std::unique_ptr<IAssetValidationRule> rule)` | Register a custom validation rule |
| `ValidateAll` | `ValidationReport ValidateAll()` | Validate all assets under `Assets/` |
| `ValidateFile` | `ValidationReport ValidateFile(const std::filesystem::path& path)` | Validate a single file |
| `ValidateDirectory` | `ValidationReport ValidateDirectory(const std::filesystem::path& dir)` | Validate a directory tree recursively |
| `GetRegisteredRules` | `std::vector<std::string_view> GetRegisteredRules() const` | List all registered rule names |
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Human-readable status |
| `Console_GetLastReport` | `std::string Console_GetLastReport() const` | Formatted last report |

### IAssetValidationRule

| Method | Signature | Description |
|--------|-----------|-------------|
| `ValidateAsset` | `virtual void ValidateAsset(const std::filesystem::path& path, ValidationReport& report) = 0` | Validate one asset, appending findings to the report |
| `GetRuleName` | `virtual std::string_view GetRuleName() const = 0` | Human-readable rule name |

## Thread Safety

`AssetValidator` is **not thread-safe**. All validation methods perform filesystem I/O and modify internal state (`m_lastReport`). Call from a single thread. The singleton access via `GetInstance()` is safe (function-local static), but concurrent calls to `ValidateAll()` or `RegisterRule()` must be externally synchronized.

Individual `IAssetValidationRule` implementations should be stateless or internally synchronized if shared across threads.

## Console Commands

Registered during engine startup (`AssetValidator::RegisterConsoleCommands()`), so the pipeline is reachable from the in-engine console:

| Command | Description |
|---------|-------------|
| `assetvalidate.status` | Show initialization state, registered rule count, and last-run summary |
| `assetvalidate.report` | Show the full findings (severity, path, message, suggestion) of the last run |
| `assetvalidate.dir <path>` | Validate every asset under a directory and print a pass/fail/warning summary |

## See Also

- [Game-Packaging](Game-Packaging.md) -- Package validated assets for distribution
- [Asset-Migration](Asset-Migration.md) -- Migrate asset formats between versions
- [Telemetry-System](../advanced/Telemetry-System.md) -- Record validation metrics
