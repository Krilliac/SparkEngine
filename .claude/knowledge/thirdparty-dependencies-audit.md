# Third-Party Dependencies Audit

**Last updated:** 2026-04-09
**Type:** Observation
**Status:** Active
**Severity:** Medium

## Description

SparkEngine now has a single source of truth for external libraries at `ThirdParty/dependencies.lock` plus a configure-time audit helper (`cmake/SparkThirdPartyAudit.cmake`).

The audit runs during CMake configure and validates:
- dependency manifest format,
- required files for each dependency,
- `.gitmodules` URL alignment for submodules,
- pinned submodule commit alignment (`git rev-parse HEAD:<path>`),
- fallback/shim metadata (`SPARK_HAS_*` style macro + stub/fallback path).

A CI guard (`tools/check-thirdparty-manifest-sync.sh`) now fails if dependency paths/URLs/version wiring changes without an accompanying `ThirdParty/dependencies.lock` update.

---

## Manifest Format (authoritative)

`ThirdParty/dependencies.lock` is a CMake-readable lock manifest:

- Defines `SPARK_THIRDPARTY_AUDIT_ENTRIES`
- One string entry per dependency
- Pipe-delimited fields:

`name|source|version_or_commit|license|local_path|required_files_csv|feature_macro|fallback_or_stub_path|severity`

Severity:
- `ERROR` → warning by default, fatal when `-DSPARK_STRICT_DEPS=ON`
- `WARN` → warning-only

---

## Current Dependency Coverage

The lock manifest currently tracks:
- submodule dependencies: miniz, EnTT, ImGui, AngelScript, RecastNavigation, SDL2,
- vendored dependencies: Jolt Physics, tinyobjloader, stb, cgltf, miniaudio, nlohmann/json, tinyexr, zstd, VulkanMemoryAllocator, glad.

Each entry includes pinned source/version metadata, SPDX-compatible license text, local path, required files, and fallback behavior.

---

## CI / Workflow Integration

- `CMakeLists.txt` invokes the audit early in configure.
- `.github/workflows/build.yml` adds `check-thirdparty-manifest` job.
- `tools/validate-all.sh` includes the manifest-sync check for local validation runs.

---

## Practical Rules

1. If you change a dependency URL, path, pinned version/commit, or required files, update `ThirdParty/dependencies.lock` in the same commit.
2. If you add/remove a dependency target in CMake, update both:
   - `ThirdParty/dependencies.lock`
   - `cmake/SparkThirdPartyAudit.cmake` expectations (if needed)
3. Keep fallback metadata accurate (`SPARK_HAS_*` behavior and stub path) so configure output matches runtime behavior.
