# Platform Certification

> **Audience:** QA | Programmers
>
> **Thread Context:** N/A (offline tooling)
>
> **Platform/Backend Scope:** Windows 11 x64, MSVC v143, D3D11 + NullRHI (stable-v1 scope)

## Overview

Platform certification (PLT-200, gate G08) proves that a specific commit builds, installs, launches, renders, and behaves correctly on every declared support-matrix row. The tooling is **fail-closed**: missing, stale, mismatched, or non-passing evidence blocks certification — silence is never treated as success.

## When to Use

- **Before a release milestone:** validate that the support matrix rows have fresh, passing evidence for the release commit.
- **After a toolchain or driver update:** re-collect evidence to confirm nothing regressed.
- **In CI or local QA:** run `validate_certification.py` to gate a build.

## Files

| Path | Purpose |
|------|---------|
| `tools/platform-cert/validate_certification.py` | Fail-closed validator (CLI + importable) |
| `tools/platform-cert/support_matrix_schema.json` | JSON Schema for the support matrix |
| `tools/platform-cert/evidence_schema.json` | JSON Schema for evidence records |
| `docs/certification/support-matrix.json` | Seed support matrix (stable-v1) |
| `docs/certification/evidence/*.json` | Per-row evidence records (one file per row) |
| `docs/certification/plans/<rowId>.json` | Collector probe plan for each declared row |
| `Tools/platform-cert/collect_evidence.py` | Runs a plan's probes and writes the measured record |
| `Tests/Tools/test_platform_certification.py` | 136 adversarial tests |

## Support Matrix

The support matrix (`docs/certification/support-matrix.json`) declares every platform row and its required evidence categories. Each row specifies:

- **id** — unique slug (e.g. `win11-x64-msvc143-d3d11`)
- **tier** — `primary` or `supported` (certifiable), `experimental` or `unsupported` (skipped)
- **os / arch / compiler / gpu / audio** — exact environment spec
- **cpuFeatures** — required CPU instruction sets
- **evidenceRequired** — which of the 13 probe categories must pass

### Current rows (stable-v1)

| Row ID | Tier | GPU API | Evidence categories |
|--------|------|---------|-------------------|
| `win11-x64-msvc143-d3d11` | primary | D3D11 | All 13 |
| `win11-x64-msvc143-nullrhi` | supported | NullRHI | 9 (no renderer/content/input/audio) |

## Evidence Records

Each evidence file captures the result of exercising a support-matrix row on a physical or CI host. Required fields:

- **commitSha** — must match the matrix's commit exactly
- **collectedAt** — ISO 8601 timestamp; must be within `maxAgeHours` (default 168h = 7 days)
- **host** — OS family/version/build/locale, arch, CPU model/features, RAM, GPU, audio, compiler (required)
- **probes** — one entry per evidence category, each with status (pass/fail/skip/error), durationMs, optional detail/artifacts

### The 13 probe categories

`build`, `install`, `launch`, `renderer`, `content`, `input`, `audio`, `save`, `crash`, `upgrade`, `rollback`, `uninstall`, `dependency_closure`

## Validation Rules (Fail-Closed)

The validator rejects a row under any of these conditions:

| Condition | Result |
|-----------|--------|
| No evidence file for a certifiable row | **FAIL** |
| Evidence `commitSha` ≠ matrix `commitSha` | **FAIL** |
| Evidence older than `maxAgeHours` | **FAIL** |
| Evidence older than its own `staleBefore` | **FAIL** |
| Missing required probe | **FAIL** |
| Probe status ≠ `pass` (fail/skip/error) | **FAIL** |
| Host OS family/version mismatch | **FAIL** |
| Host arch mismatch | **FAIL** |
| Host GPU API mismatch | **FAIL** |
| Host compiler ID or toolset mismatch | **FAIL** |
| Host missing required CPU features | **FAIL** |
| Host audio API mismatch | **FAIL** |
| Schema validation failure | **FAIL** |
| Duplicate JSON keys in any file | **FAIL** |
| NaN/Infinity in any numeric field | **FAIL** |
| Extra fields (additionalProperties) at any level | **FAIL** |
| Timestamps before 2020 or beyond 5-min clock skew | **FAIL** |
| Pass probe with zero duration (< 1ms) | **FAIL** |
| SHA-256 not 64 lowercase hex | **FAIL** |
| Artifact path absolute, traversal, or backslash | **FAIL** |
| Missing artifact sizeBytes | **FAIL** |
| Canonical profile (stable-v1) row missing/unexpected | **FAIL** |
| GPU vendor/device/driver/featureLevel mismatch | **FAIL** |
| Compiler version/toolset mismatch | **FAIL** |
| Missing dependency closure when required | **FAIL** |
| Collector identity blank or missing | **FAIL** |
| Resource limits exceeded (rows, deps, strings, etc.) | **FAIL** |

Rows with `tier=experimental` or `tier=unsupported` are skipped with a warning — they do not block certification.

## CLI Usage

```bash
# Full validation (matrix + evidence)
python tools/platform-cert/validate_certification.py

# Matrix-only schema check
python tools/platform-cert/validate_certification.py --matrix-only

# Custom paths and staleness
python tools/platform-cert/validate_certification.py \
  --matrix docs/certification/support-matrix.json \
  --evidence-dir docs/certification/evidence \
  --max-age-hours 72
```

Exit code 0 = all certifiable rows pass. Exit code 1 = at least one failure.

## Collecting Evidence

Evidence is not fabricated — it must come from actual test runs on matching hardware. The workflow:

1. Build the engine at the exact commit declared in the matrix.
2. Run each probe category on the target hardware.
3. Record results in a JSON file following `evidence_schema.json`.
4. Place the file in `docs/certification/evidence/` named `{rowId}.json`.
5. Run the validator to confirm the evidence passes.

### Row probe plans

Each declared row has a collector plan in `docs/certification/plans/`. Every probe runs a
command that already exists; categories with no implementation are listed under
`uncoveredCategories` with the reason, never mapped to a stand-in that could pass.

| Row | Probed | Command |
|-----|--------|---------|
| `win11-x64-msvc143-nullrhi` | `build` | `cmake --build --preset windows-shipping --config MinSizeRel` |
| | `launch` | `ctest --test-dir build/windows-shipping -C MinSizeRel -R ^NullRHI_Windows_FPSLifecycle$ --no-tests=error` |
| `win11-x64-msvc143-d3d11` | `build` | same Shipping build |
| | `content`, `save` | `ctest ... -R ^FPSPackage_InstalledRuntime$ --no-tests=error` |

Uncovered today: `install`/`uninstall`/`upgrade`/`rollback` (the MSI qualifier needs per-run
arguments and predecessor packages; INST-130, REL-100 and REL-110), `crash` (OPS-100), and
`dependency_closure` (it must come from the staged binaries' PE import tables). The NullRHI row
also leaves out `save`, because the only save/reload proof runs D3D11 WARP. The D3D11 row also
leaves out `launch` and `renderer`, because every D3D11 test forces WARP, plus `input` and
`audio`. The validator refuses to certify a row with any category missing, so both rows stay
uncertified until these gaps close.

Plans carry no `provenance` block. Collection therefore has to run under CI with
`--from-github-env` on the physical host, after
`cmake --preset windows-shipping -DBUILD_TESTS=ON` at the commit under test. Probes run in
category order, so `build` rebuilds the product before any ctest probe uses it.
`TestRowProbePlans` in `Tests/Tools/test_platform_certification.py` checks four things: every
probed or uncovered category is in the row's `evidenceRequired`, every `ctest -R ^Name$`
selector names an `add_test` in `Tests/CMakeLists.txt`, every ctest probe passes
`--no-tests=error`, and every named preset exists in `CMakePresets.json`.

### Remaining blockers for PLT-200 completion

The certification infrastructure is in place. What remains is **physical-host evidence collection** — an operator must:

- Build, install, launch, and exercise SparkEngine on a Windows 11 x64 machine with an NVIDIA GPU (D3D11 row)
- Build, install, launch, and exercise SparkEngine in NullRHI headless mode (NullRHI row)
- Record the results as evidence JSON files
- Run the validator to confirm pass

This cannot be automated away — it requires real hardware running real builds.

## Running Tests

```bash
python -m pytest Tests/Tools/test_platform_certification.py -v
```

136 tests covering schema loading, strict JSON parsing (duplicate keys, NaN/Infinity), additionalProperties enforcement, timestamp bounds, compiler identity, zero-duration pass rejection, SHA-256/artifact path confinement, canonical profile coverage, complete host matching, dependency closure, collector identity, resource limits, cross-validation, and CLI modes.

## Windows installer predecessor evidence

The Runtime MSI family uses the project-owned UpgradeCode declared in
`cmake/SparkCPackOptions.cmake`; ProductCode remains generated per MSI. Before
an old-to-new transaction, `.github/scripts/qualify-windows-msi.py` requires
`--previous-signer-thumbprint` from the caller's protected trust configuration,
together with all three predecessor artifact arguments. It verifies a valid,
timestamped Authenticode signature from that publisher on the private MSI copy
before any Windows Installer command, and keeps that copy locked through its
initial installation. The current MSI is locked and its digest rechecked before
the `/fvomus` repair command; the lock remains held until that command exits.
`previous-signature.json` binds the checked publisher
and native signature result to the predecessor bytes; it is signature evidence,
not proof that the installation or upgrade passed.

This foundation does not admit a bootstrap release, provision signing authority,
or certify Windows 11. The release controller supplies the externally provisioned
`SPARK_PREVIOUS_RELEASE_SIGNER_THUMBPRINT` repository/environment variable explicitly
to the qualifier. The release owner must set it to the authorized predecessor
publisher's 40-hex Authenticode certificate thumbprint; it is not inferred from
the downloaded MSI or the current release's signer. An absent, malformed, or
mismatched value blocks qualification. Hosted signed-artifact and supported-host
execution remain required evidence.

## Related Pages

- [System Requirements](System-Requirements.md)
- [CI Reproducible Builds](../development/CI-Reproducible-Builds.md)
- [Testing](../advanced/Testing.md)

## Source & Freshness

- **Created:** 2026-08-28 for PLT-200
- **Commit:** `360c05e883d4d5d1c0d050455a5cbb226cce3ffc`
- **Status:** Infrastructure complete; evidence collection pending
