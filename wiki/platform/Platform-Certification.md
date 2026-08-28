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
| `Tests/Tools/test_platform_certification.py` | 77 adversarial tests |

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
- **host** — OS family/version/build/locale, arch, CPU model/features, RAM, GPU, audio, optionally compiler
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

77 tests covering schema validation, cross-validation, evidence tampering, staleness, host mismatches, tampered commits, and full-pipeline certification.

## Related Pages

- [System Requirements](System-Requirements.md)
- [CI Reproducible Builds](../development/CI-Reproducible-Builds.md)
- [Testing](../advanced/Testing.md)

## Source & Freshness

- **Created:** 2026-08-28 for PLT-200
- **Commit:** `360c05e883d4d5d1c0d050455a5cbb226cce3ffc`
- **Status:** Infrastructure complete; evidence collection pending
