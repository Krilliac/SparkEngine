# Migration Guide

How to migrate between SparkEngine source revisions and handle breaking changes.
No versioned stable release currently establishes a public compatibility policy;
the declared `stable-v1` profile remains blocked and uncertified.

---

## Source Version Fields

Source headers carry `MAJOR.MINOR.PATCH` fields and `GetSDKVersion()` values for loader and source-migration checks. Until a versioned release exists, those fields are versioned but uncertified: they do not promise public source, SDK, or binary compatibility across `Working` revisions.

- **MAJOR** — Source changes may include breaking API changes (for example, removed public methods or changed signatures)
- **MINOR** — Source changes may add features
- **PATCH** — Source changes may include fixes or documentation updates

Check the current engine version via `IModule::GetSDKVersion()` or the `SPARK_ENGINE_VERSION` macro in `SparkSDK/SparkSDK.h`.

---

## Source Compatibility Boundaries

| Component | Stability |
|-----------|-----------|
| SparkSDK public headers | Versioned source interface; rebuild against the target revision; no released SDK stability promise |
| IModule interface | Loader SDK-version check; rebuild for the target revision; no released ABI promise |
| Wire format (networking) | Source-versioned where specified; uncertified until a release contract exists |
| Asset formats | Source-versioned where specified; validate migration against the target revision |
| Plugin ABI | Source ABI fields/checks; rebuild against target headers; no released ABI stability promise |
| Internal engine headers | No stability guarantee |
| Console commands | May change between minor versions |

---

## Breaking Change Policy

A **breaking change** is any of the following:

1. Removing or renaming a public method/class in `SparkSDK/`
2. Changing the signature of a virtual method in `IModule`, `IRHIDevice`, or `ITransport`
3. Changing the networking wire format without a version bump
4. Changing the asset binary format without migration support
5. Removing a CMake option or changing its default value

Until a versioned release policy exists, treat any such source change as a migration trigger: pin the target revision, rebuild consumers, and validate them there. Working documentation may record known changes, but it does not establish release-note, deprecation-period, or stable-compatibility guarantees.

---

## Handling Deprecated APIs

SparkEngine uses deprecation macros:

```cpp
// Deprecated method — will be removed in next major version
SPARK_DEPRECATED("Use NewMethod() instead")
void OldMethod();
```

When you see deprecation warnings:
1. Read the deprecation message for the replacement API
2. Update your code to use the new API
3. Test with the new API before upgrading

---

## Asset Format Versioning

Asset files contain a version header. When the engine loads an asset:

1. It reads the version from the file header
2. If the version is older than current, `AssetMigration` runs automatic upgrade
3. Upgraded assets are written back to disk (or cached in memory)

See `SparkEngine/Source/Core/AssetMigration.h` for the migration registry and `docs/specs/asset-format.md` for format details.

### Forcing re-migration

```
spark_console asset.reimport_all
```

---

## Plugin ABI and Source Migration

Game modules (DLLs) are loaded dynamically. Current source uses SDK/ABI version checks to reject incompatible combinations; it does not certify a stable public SDK, `IModule`, or plugin ABI before a versioned release. For a source migration, keep these build constraints aligned:

- Same compiler family and major version (e.g., MSVC v143)
- Same C++ standard library
- Same `IModule` interface version (checked at load time)

When moving to another source revision:
1. Record the target commit and check `IModule::GetSDKVersion()` against that engine
2. Recompile all game modules and plugins against the target SDK headers
3. Run the registered `SparkEngineTests` aggregate for that configuration:
   `ctest --test-dir build -C Release -R "^SparkEngineTests$" --output-on-failure --no-tests=error`.
   There is no `GameModule` CTest suite name; use this registered aggregate or
   invoke `SparkTests` directly with its documented `SPARK_TEST_FILE` or
   `SPARK_TEST_NAME` filters when narrowing a diagnosis.

See `docs/specs/plugin-abi-guide.md` for the current source ABI layout, not a release compatibility guarantee.

---

## Step-by-Step Upgrade Checklist

### Before upgrading

- [ ] Back up your project (or ensure clean git state)
- [ ] Read the release notes for all versions between current and target
- [ ] Note any breaking changes or deprecations

### During upgrade

- [ ] Update engine source to the chosen commit or versioned-but-uncertified source snapshot
- [ ] Update SparkSDK headers in your project
- [ ] Fix any compilation errors from removed/changed APIs
- [ ] Address deprecation warnings
- [ ] Re-run `cmake --preset <your-preset>` to pick up new options
- [ ] Rebuild: `cmake --build build --config Release`

### After upgrading

- [ ] Run all tests: `ctest --test-dir build -C Release --output-on-failure --no-tests=error`
- [ ] Run your game module tests
- [ ] Test asset loading — watch for migration warnings in the log
- [ ] Verify networking compatibility if running multiplayer
- [ ] Check console commands still work (some may have been renamed)
- [ ] Update any custom build scripts or CI pipelines

---

## Version History

Until versioned releases are published, use the repository [changelog](../Changelog.md) and compare the exact source commits being migrated.
