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
| `Tools/platform-cert/pe_imports.py` | Bounded PE32+ import/delay-import reader; measures a staged package's dependency closure |
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
| `dependency_closure` probe without exactly one PE import graph (`*.imports.json`) | **FAIL** |
| Measured non-API-set import missing from the closure, or a closure entry nothing imports | **FAIL** |
| Import resolving neither to the package root nor to an authority `platformRuntime` entry | **FAIL** |
| Import graph whose API-set/platform classification disagrees with the authority | **FAIL** |
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
`dependency_closure` (the measurement exists, see below, but no MSVC package has been walked, so
no plan can yet declare the closure it would be compared against). The NullRHI row
also leaves out `save`, because the only save/reload proof runs D3D11 WARP. The D3D11 row also
leaves out `launch` and `renderer`, because every D3D11 test forces WARP, plus `input` and
`audio`. The validator refuses to certify a row with any category missing, so both rows stay
uncertified until these gaps close.

### Measured dependency closure

`collect_evidence.py --package-root <staged package>` records `dependency_closure` by running
`pe_imports.py` as the probe subprocess. It reads the import and delay-import directories of every
PE32+ (AMD64) image in the package and resolves each DLL name. An `api-ms-win-*`/`ext-ms-*` name is
an OS API set. A KnownDLL (`KNOWN_DLLS` in `pe_imports.py`, such as `kernel32.dll`) always loads from
the system directory, so a same-named file in the package never satisfies it. Any other DLL in the
package root (the application directory) is package-local and hashed. Anything else must be a
`platformRuntime` entry of `docs/certification/dependency-authority.json`, or it is unresolved. The canonical import graph becomes a content-addressed `*.imports.json`
artifact, and each declared dependency gets its own evidence file under `dependency_closure/deps/`.

The plan declares only `dependencyClosure` entries (`name`, `version`, `source`) and the product's own
`firstPartyImages`. Paths, hashes and sizes are always measured, and a plan that declares a closure
without `--package-root` is refused. The validator re-reads the attested graph and fails the row if
any of these holds:

- an import is unresolved
- a measured non-API-set, non-first-party import is missing from the closure
- a closure entry is imported by nothing
- an API set is declared
- the graph's name-only classifications disagree with the authority
- a package-local DLL, or a `firstPartyImages` entry, has a name the OS owns (an API set, a
  KnownDLL or a `system`-source `platformRuntime` library)
- a `firstPartyImages` entry names a `platformRuntime` library or a third-party image
- a package-local DLL is neither first-party, an app-local `platformRuntime` library (declared under
  its own name and source, e.g. `msvcp140.dll` from `vcredist`), nor a reviewed third-party image

A package-local third-party DLL is declared under its authority identity, not its file name. The
graph records file names (`sdl2.dll`) while the authority's `thirdParty` entries are manifest projects
(`SDL2`), so the link is the entry's reviewed `imageNames` list. That list comes from
`THIRD_PARTY_PACKAGE_IMAGES` in `dependency_authority.py`. It is empty today because every vendored
library is linked statically into the Windows binaries (SDL2 is only built off Windows). A Windows
package that ships a third-party DLL is therefore refused until that DLL is reviewed into the table
and the authority is regenerated.

Not every classification is re-derived at validation time. Whether a DLL sits in the package root
depends on the package, which the bundle does not carry. The validator trusts that fact from the
attested collector run and pins it only to a package-root image of the same name. The plan's
`firstPartyImages` list is also a declaration. It is checked against OS-owned, runtime and
third-party names, but not against the build's real targets.

The reader is bounded: 512 MiB per image, 96 sections, 16 data directories, 4096 descriptors per
directory, and 255-byte names. Truncated headers, an RVA outside every section, PE32 or non-AMD64
images, and names containing path characters all fail closed.

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

The v0.9.0 bootstrap path (`--bootstrap-repair`) has no predecessor, so it binds
its evidence to the owner-reviewed baseline instead. It requires
`--reviewed-baseline-commit`, which `release.yml` reads from
`predecessorRelease.sourceCommitEvidence.baselineCommit` in
`docs/site/readiness.json`. Before any Windows Installer command, the source SHA
must have exactly one parent equal to that baseline. This is the same rule the
publication job's `verify_v090_source_seal.py` enforces, and the qualifier
imports it from that script instead of keeping a copy. Both SHAs are
written to `bootstrap-baseline.json`. While the baseline is unrecorded (empty),
the v0.9.0 qualification step fails closed.

## Related Pages

- [System Requirements](System-Requirements.md)
- [CI Reproducible Builds](../development/CI-Reproducible-Builds.md)
- [Testing](../advanced/Testing.md)

## Source & Freshness

- **Created:** 2026-08-28 for PLT-200
- **Commit:** `360c05e883d4d5d1c0d050455a5cbb226cce3ffc`
- **Status:** Infrastructure complete; evidence collection pending
