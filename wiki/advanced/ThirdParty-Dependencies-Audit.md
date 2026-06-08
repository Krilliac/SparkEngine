# ThirdParty Dependencies Audit

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (audit/reference)
>
> **Platform/Backend Scope:** All platforms / all backends

## Overview

SparkEngine has a single source of truth for external libraries at `ThirdParty/dependencies.lock`, plus a configure-time audit helper (`cmake/SparkThirdPartyAudit.cmake`) and a CI guard (`tools/check-thirdparty-manifest-sync.sh`). The lock manifest pins source, version/commit, license, local path, required files, feature macro, and fallback behavior for every dependency, and CMake validates the manifest during configure. All three pieces are confirmed present as of 2026-06-08.

---

## What the Audit Validates

The audit runs during CMake configure and checks:

- dependency manifest format,
- required files for each dependency,
- `.gitmodules` URL alignment for submodules,
- pinned submodule commit alignment (`git rev-parse HEAD:<path>`),
- fallback / shim metadata (a `SPARK_HAS_*`-style macro plus a stub/fallback path).

The CI guard `tools/check-thirdparty-manifest-sync.sh` fails if dependency paths/URLs/version wiring change without a matching `ThirdParty/dependencies.lock` update.

---

## Manifest Format (authoritative)

`ThirdParty/dependencies.lock` is a CMake-readable lock manifest. It defines `SPARK_THIRDPARTY_AUDIT_ENTRIES`, one pipe-delimited string per dependency:

```
name|source|version_or_commit|license|local_path|required_files_csv|feature_macro|fallback_or_stub_path|severity
```

Severity:

- `ERROR` → warning by default, fatal when `-DSPARK_STRICT_DEPS=ON`.
- `WARN` → warning-only.

---

## Currently Tracked Dependencies (16 entries, lock last-synced 2026-04-20)

| Dependency | Version / commit | License | Severity | Feature macro |
|------------|------------------|---------|----------|---------------|
| Jolt Physics | v5.5.1 (vendored snapshot) | MIT | ERROR | `SPARK_JOLT_PHYSICS_AVAILABLE` |
| miniz | commit `4b9fcf1d…` | MIT | ERROR | `SPARK_HAS_MINIZ` |
| EnTT | commit `9c5281c7…` | MIT | WARN | `SPARK_HAS_ENTT` |
| Dear ImGui | docking branch `148bd34a…` | MIT | WARN | `SPARK_HAS_IMGUI` |
| AngelScript | mirror `c81df254…` | zlib | WARN | `SPARK_HAS_ANGELSCRIPT` |
| RecastNavigation | commit `9f4ce644…` | zlib | WARN | `SPARK_RECAST_AVAILABLE` |
| SDL2 | commit `859844ea…` (SDL 2.30.0) | zlib | WARN | `SPARK_HAS_SDL2` |
| tinyobjloader | v2.0.0 (header snapshot) | MIT | WARN | `SPARK_HAS_TINYOBJLOADER` |
| stb_image | snapshot blob | Public Domain / MIT | WARN | `SPARK_HAS_STB_IMAGE` |
| cgltf | snapshot blob | MIT | WARN | `SPARK_HAS_CGLTF` |
| miniaudio | snapshot blob | Public Domain / MIT-0 | WARN | `SPARK_HAS_MINIAUDIO` |
| nlohmann/json | v3.11.3 (single-header) | MIT | WARN | `SPARK_HAS_NLOHMANN_JSON` |
| tinyexr | v1.0.9 (single-header) | BSD-3-Clause | WARN | `SPARK_HAS_TINYEXR` |
| zstd | v1.5.6 (vendored wrapper) | BSD-3-Clause | WARN | `SPARK_HAS_ZSTD` |
| VulkanMemoryAllocator | snapshot blob | MIT | WARN | `SPARK_HAS_VMA` |
| glad | 0.1.36 (generated loader snapshot) | MIT | WARN | `SPARK_OPENGL_SUPPORT` |

Each entry pins its source/version, SPDX-compatible license, local path, required files, and a fallback path so a missing dependency degrades gracefully (e.g. Jolt → `PhysicsSystemStub.cpp`, SDL2 → headless mode, stb_image → DDS-only textures, VMA → internal allocator).

### Submodules vs. vendored snapshots

- **Submodules** (pinned by commit, validated against `.gitmodules`): miniz, EnTT, ImGui, AngelScript, RecastNavigation, SDL2.
- **Vendored snapshots** (committed blobs/headers in-tree): Jolt Physics, tinyobjloader, stb, cgltf, miniaudio, nlohmann/json, tinyexr, zstd, VulkanMemoryAllocator, glad.

> The original audit text described "submodule dependencies: miniz, EnTT, ImGui, AngelScript, RecastNavigation, SDL2; vendored: Jolt, tinyobjloader, stb, cgltf, miniaudio, nlohmann/json, tinyexr, zstd, VulkanMemoryAllocator, glad." That split is unchanged — re-verified directly against the current lock file.

---

## CI / Workflow Integration

- `CMakeLists.txt` invokes the audit early during configure.
- `.github/workflows/build.yml` runs a `check-thirdparty-manifest` job.
- `tools/validate-all.sh` includes the manifest-sync check for local validation.

---

## Practical Rules

1. If you change a dependency URL, path, pinned version/commit, or required files, update `ThirdParty/dependencies.lock` in the same commit.
2. If you add/remove a dependency target in CMake, update both `ThirdParty/dependencies.lock` and `cmake/SparkThirdPartyAudit.cmake` expectations as needed.
3. Keep fallback metadata accurate (`SPARK_HAS_*` behavior and stub path) so configure output matches runtime behavior.
4. After cloning, run `git submodule update --init --recursive` before CMake configure.

---

## Source & Freshness

- **Original audit:** `.claude/knowledge/thirdparty-dependencies-audit.md`, last updated 2026-04-09.
- **Re-measured against codebase 2026-06-08.**
- OLD → NEW notes:
  - Confirmed `ThirdParty/dependencies.lock`, `cmake/SparkThirdPartyAudit.cmake`, and `tools/check-thirdparty-manifest-sync.sh` all still exist.
  - Added the concrete per-dependency table (16 entries) read directly from the current lock file — the original audit listed dependencies in prose only.
  - Lock-file `Last sync` header is now 2026-04-20 (post-dating the original audit), reflecting an SDL2 branch-pin + CMake split; submodule pointer unchanged (SDL 2.30.0).
  - Submodule vs. vendored split re-verified as unchanged.
- Findings now resolved/changed since the original audit: none broken — the manifest and guards are intact and current; the only change is the lock's own sync date advancing.

## Related Pages

- [Build System and CMake Modules](Build-System-and-CMake-Modules.md)
- [Codebase Observations](Codebase-Observations.md)
- [Contributing](Contributing.md)
