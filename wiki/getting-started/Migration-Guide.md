# Migration Guide

How to upgrade between SparkEngine versions and handle breaking changes.

---

## Version Numbering

SparkEngine uses **semantic versioning** (`MAJOR.MINOR.PATCH`):

- **MAJOR** — Breaking API changes (e.g., removed public methods, changed signatures)
- **MINOR** — New features, backward-compatible additions
- **PATCH** — Bug fixes, documentation updates

Check the current engine version via `IModule::GetSDKVersion()` or the `SPARK_ENGINE_VERSION` macro in `SparkSDK/SparkSDK.h`.

---

## Compatibility Promises

| Component | Stability |
|-----------|-----------|
| SparkSDK public headers | Stable across minor versions |
| IModule interface | Stable (ABI-compatible within major version) |
| Wire format (networking) | Versioned — see `docs/specs/networking-wire-format.md` |
| Asset formats | Versioned — see `docs/specs/asset-format.md` |
| Plugin ABI | Stable within major version — see `docs/specs/plugin-abi-guide.md` |
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

Breaking changes are:
- Announced in release notes with migration instructions
- Accompanied by a deprecation period (minimum one minor version)
- Documented in this guide

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

## Plugin ABI Stability

Game modules (DLLs) are loaded dynamically. ABI compatibility requires:

- Same compiler family and major version (e.g., MSVC v143)
- Same C++ standard library
- Same `IModule` interface version (checked at load time)

When upgrading the engine:
1. Check `IModule::GetSDKVersion()` matches the engine
2. Recompile all game modules against the new SDK headers
3. Run module tests: `ctest --test-dir build -R GameModule`

See `docs/specs/plugin-abi-guide.md` for full ABI details.

---

## Step-by-Step Upgrade Checklist

### Before upgrading

- [ ] Back up your project (or ensure clean git state)
- [ ] Read the release notes for all versions between current and target
- [ ] Note any breaking changes or deprecations

### During upgrade

- [ ] Update engine source (git pull / download release)
- [ ] Update SparkSDK headers in your project
- [ ] Fix any compilation errors from removed/changed APIs
- [ ] Address deprecation warnings
- [ ] Re-run `cmake --preset <your-preset>` to pick up new options
- [ ] Rebuild: `cmake --build build --config Release`

### After upgrading

- [ ] Run all tests: `ctest --test-dir build --output-on-failure`
- [ ] Run your game module tests
- [ ] Test asset loading — watch for migration warnings in the log
- [ ] Verify networking compatibility if running multiplayer
- [ ] Check console commands still work (some may have been renamed)
- [ ] Update any custom build scripts or CI pipelines

---

## Version History

See the [release notes](https://github.com/Krilliac/SparkEngine/releases) for detailed changelogs.
