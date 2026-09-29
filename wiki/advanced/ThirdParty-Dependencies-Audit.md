# ThirdParty Dependencies Audit

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (audit/reference)
>
> **Platform/Backend Scope:** All platforms / all backends

## Overview

SparkEngine has a single source of truth for external libraries at `ThirdParty/dependencies.lock`, plus a configure-time audit helper (`cmake/SparkThirdPartyAudit.cmake`) and a CI guard (`tools/check-thirdparty-manifest-sync.sh`). The lock manifest pins source, version/commit, license, local path, required files, feature macro, and fallback behavior for every dependency, and CMake validates the manifest during configure. Vendored snapshots carry explicit versions in the manifest; submodule versions resolve from the immutable mode-160000 gitlinks in the repository tree.

---

## What the Audit Validates

The audit runs during CMake configure and checks:

- dependency manifest format,
- required files for each dependency,
- `.gitmodules` URL alignment for submodules,
- pinned submodule commit alignment (manifest rendering and `git rev-parse HEAD:<path>` both use the repository gitlink),
- fallback / shim metadata (a `SPARK_HAS_*`-style macro plus a stub/fallback path).

The CI guard `tools/check-thirdparty-manifest-sync.sh` fails if dependency paths/URLs/version wiring change without a matching `ThirdParty/dependencies.lock` update. The only exception is a verified, same-repository Dependabot pull request whose complete diff consists solely of existing `ThirdParty/` gitlinks advancing from one mode-160000 commit to another; the new gitlink is already the canonical lock value.

### Reviewed exceptions

`ThirdParty/supply-chain.lock` carries an `exceptions` list for explicitly
reviewed, temporary risks. The list is normally empty and each record must have
exactly `id`, `scope`, `owner`, `justification`, and `expires` fields. IDs are
case-insensitively unique, the owner must name a maintainer, and the expiry must
be a valid ISO date. Malformed records fail the checker itself; expired records
are policy violations. These records document review only and never suppress
inventory, hash, license, action-pin, or manifest checks.

---

## Manifest Format (authoritative)

`ThirdParty/dependencies.lock` is a CMake-readable lock manifest. It defines `SPARK_THIRDPARTY_AUDIT_ENTRIES`, one pipe-delimited string per dependency:

```
name|source|version_or_commit|license|local_path|required_files_csv|feature_macro|fallback_or_stub_path|severity
```

Severity (default configure, `SPARK_STRICT_DEPS=OFF`):

- `ERROR` → warning that suggests `-DSPARK_STRICT_DEPS=ON`.
- `WARN` → warning.

**Strict-dependency closure.** With `-DSPARK_STRICT_DEPS=ON` (the `windows-release`, `windows-shipping` and
`linux-shipping` presets, and the release workflow) every entry in this manifest is required, whatever its severity.
A missing path or required file, or a submodule URL or gitlink revision that does not match, is collected, and the
audit ends configure with one `FATAL_ERROR` that lists every issue. The manifest is the only strict list: root
`CMakeLists.txt` no longer carries its own Jolt/ImGui/EnTT checks, so adding an entry here extends the strict closure.
`StrictDependencies_LockClosureMissingIsFatal` (`Tests/Tools/test_strict_dependencies.py`) configures a fixture
project against the real manifest and audit module. It removes each locked dependency in turn, requires a strict
failure that names the dependency, and requires that a non-strict configure only warns.

---

## Currently Tracked Dependencies (16 entries, verified 2026-08-26)

| Dependency | Version / commit | License | Severity | Feature macro |
|------------|------------------|---------|----------|---------------|
| Jolt Physics | v5.5.1 (vendored snapshot) | MIT | ERROR | `SPARK_JOLT_PHYSICS_AVAILABLE` |
| miniz | gitlink `77d0dce8…` | MIT | ERROR | `SPARK_HAS_MINIZ` |
| EnTT | gitlink `85c6bba0…` | MIT | WARN | `SPARK_HAS_ENTT` |
| Dear ImGui | docking gitlink `fd13a1e8…` | MIT | WARN | `SPARK_HAS_IMGUI` |
| AngelScript | mirror `c81df254…` | zlib | WARN | `SPARK_HAS_ANGELSCRIPT` |
| RecastNavigation | commit `9f4ce644…` | zlib | WARN | `SPARK_RECAST_AVAILABLE` |
| SDL2 | gitlink `5882a4f1…` | zlib | WARN | `SPARK_HAS_SDL2` |
| tinyobjloader | v2.0.0 (header snapshot) | MIT | WARN | `SPARK_HAS_TINYOBJLOADER` |
| stb_image | snapshot blob | Public Domain / MIT | WARN | `SPARK_HAS_STB_IMAGE` |
| cgltf | snapshot blob | MIT | WARN | `SPARK_HAS_CGLTF` |
| miniaudio | snapshot blob | Public Domain / MIT-0 | WARN | `SPARK_HAS_MINIAUDIO` |
| nlohmann/json | v3.11.3 (single-header) | MIT | WARN | `SPARK_HAS_NLOHMANN_JSON` |
| tinyexr | v1.0.13 (hardened official snapshot) | BSD-3-Clause | WARN | `SPARK_HAS_TINYEXR` |
| zstd | v1.5.6 (vendored wrapper) | BSD-3-Clause | WARN | `SPARK_HAS_ZSTD` |
| VulkanMemoryAllocator | snapshot blob | MIT | WARN | `SPARK_HAS_VMA` |
| glad | 0.1.36 (generated loader snapshot) | MIT | WARN | `SPARK_OPENGL_SUPPORT` |

Each entry pins its source/version, declared license field, local path, required files, and a fallback path so a missing dependency degrades gracefully (e.g. Jolt → `PhysicsSystemStub.cpp`, SDL2 → headless mode, stb_image → DDS-only textures, VMA → internal allocator). The free-text or choice-based license fields remain subject to GOV-400 owner/legal review; this audit does not convert them into legal classifications. In a Git checkout, each submodule version field is rendered from `git ls-tree HEAD`; source archives without repository metadata retain a descriptive gitlink marker and are still validated by their bundled files.

## Packaged notice coverage

`tools/governance/generate_third_party_notices.py` reproduces only license text
that is present on disk. It fails closed for missing notice files, undeclared
license files, repository-authored stubs, malformed manifests, and non-SPDX
choice fields. The generated repository notice is checked for freshness.

Install trees are checked by `cmake/ValidateStagedPackageNotices.cmake` using
the shared `cmake/PackageNoticeCoverageRules.json` rules. Every shipped
ThirdParty-derived payload must map to a declared inventory entry, and every
editor font must be named in the packaged `THIRD_PARTY_NOTICES.txt` with its
reproduced license text. Coverage is an inventory check; it is not legal or
maintainer sign-off. Run the package checker separately on the actual Windows
and Linux install roots; a source-tree inventory pass cannot establish their
packaged coverage.

The legal validator also checks the support tables in `SECURITY.md` and
`SUPPORT.md` against the configured development channel and the unpublished
release-profile boundary (`tools/site-data/policy.py`). Local Git tags do not
establish publication. Supported-version wording needs reviewed publication
evidence before this conservative guard can be extended.

### Submodules vs. vendored snapshots

- **Submodules** (pinned by repository gitlink, validated against `.gitmodules`): miniz, EnTT, ImGui, AngelScript, RecastNavigation, SDL2.
- **Vendored snapshots** (committed blobs/headers in-tree): Jolt Physics, tinyobjloader, stb, cgltf, miniaudio, nlohmann/json, tinyexr, zstd, VulkanMemoryAllocator, glad.

> The original audit text described "submodule dependencies: miniz, EnTT, ImGui, AngelScript, RecastNavigation, SDL2; vendored: Jolt, tinyobjloader, stb, cgltf, miniaudio, nlohmann/json, tinyexr, zstd, VulkanMemoryAllocator, glad." That split is unchanged — re-verified directly against the current lock file.

---

## CI / Workflow Integration

- `CMakeLists.txt` invokes the audit early during configure.
- `.github/workflows/build.yml` runs a `check-thirdparty-manifest` job.
- `.github/workflows/build.yml` also runs the required `license-compliance` job,
  which executes the legal contract validator.
- `tools/validate-all.sh` includes the manifest-sync check for local validation.
- Dependabot runs weekly for GitHub Actions and git submodules. Pointer-only submodule PRs pass only after event identity, same-repository origin, raw gitlink modes, `.gitmodules` membership, and manifest membership are all verified.

---

## Practical Rules

1. If you manually change a dependency URL, path, pinned version/commit, or required files, update `ThirdParty/dependencies.lock` in the same commit. The narrow Dependabot pointer-only rule is automated and does not apply to human or mixed diffs.
2. If you add/remove a dependency target in CMake, update both `ThirdParty/dependencies.lock` and `cmake/SparkThirdPartyAudit.cmake` expectations as needed.
3. Keep fallback metadata accurate (`SPARK_HAS_*` behavior and stub path) so configure output matches runtime behavior.
4. After cloning, run `git submodule update --init --recursive` before CMake configure.

---

## Source & Freshness

- **Original audit:** `.claude/knowledge/thirdparty-dependencies-audit.md`, last updated 2026-04-09.
- **Re-measured against codebase 2026-09-13.**
- OLD → NEW notes:
  - Confirmed `ThirdParty/dependencies.lock`, `cmake/SparkThirdPartyAudit.cmake`, and `tools/check-thirdparty-manifest-sync.sh` all still exist.
  - Added the concrete per-dependency table (16 entries) read directly from the current lock file — the original audit listed dependencies in prose only.
  - Repository gitlinks are now canonical for the six submodule revisions, eliminating duplicate SHA drift in Dependabot PRs while preserving the manifest guard for every other dependency change.
  - Submodule vs. vendored split re-verified as unchanged.
  - Added the fail-closed reviewed-exception schema and required legal-compliance
    CI coverage.
- 2026-09-26 (CI-120): `SPARK_STRICT_DEPS=ON` now makes the whole manifest the strict closure (both severities, one
  aggregated `FATAL_ERROR`). The hard-coded Jolt/ImGui/EnTT strict checks were removed from root `CMakeLists.txt`.
- Findings now resolved/changed since the original audit: submodule pointer updates no longer require Dependabot to edit a second SHA copy, and the CI guard now verifies the bot/event/diff shape before allowing that narrow path.

## Related Pages

- [Build System and CMake Modules](Build-System-and-CMake-Modules.md)
- [Codebase Observations](Codebase-Observations.md)
- [Contributing](Contributing.md)
