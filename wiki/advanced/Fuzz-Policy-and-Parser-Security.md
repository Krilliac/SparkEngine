# Fuzz Policy and Parser Security

**Audience:** Engine developers, security reviewers, and CI maintainers
**Thread Context:** SEC-120 — stable-v1 untrusted file and package parsers
**Platform-Backend Scope:** Cross-platform policy tooling; Linux CI is the blocking execution lane

## Current Status

SEC-120 remains open and release-blocking. The repository has a blocking structural
policy gate, but it does not yet have a production fuzz target, seed corpus, sanitizer
fuzz smoke, scheduled campaign, coverage result, or crash-free-duration result.

The deterministic snapshot in `docs/sec120-fuzz-policy-check.json` is validated by CI.
For the recorded source-tree state it reports **108 explicitly inventoried parsers, all
blocked**, **149 detected candidates deferred with an owner and expiry**, and **1963
source files scanned across 17 first-party roots**. Those counts are not fuzz coverage.
`passed` in that snapshot is computed from the closure blockers, so it reads `false`
while any blocker remains.

Two gates, deliberately separate:

| Gate | Command | Blocks | Today |
|---|---|---|---|
| Structural | `check_fuzz_policy.py --ci` | merges (Required CI Gate) | passes |
| Closure | `check_fuzz_policy.py --ci --require-closure` | releases (`release.yml`) | **fails — by design** |

Network packet/protocol fuzzing belongs to NET-100 behind G12. AngelScript and
visual-script fuzzing belongs to ENG-200 behind G11, including `.as` script-file
ingestion under `SparkEngine/Source/Engine/Scripting`. Their subtrees are named as
ticketed, owned, expiring exclusions rather than silently omitted.

## What the Gate Proves

The policy tooling lives under the case-sensitive Git path `tools/fuzz-policy/`.
The `fuzz-policy` Linux job runs its standalone CMake project, and Required CI Gate
depends on that job. The root CMake project exposes `check-fuzz-policy` and the
`FuzzPolicy`/`FuzzPolicyAdversarial` CTest tests behind
`SPARK_ENABLE_FUZZ_POLICY_CHECKS` (default `${BUILD_TESTS}`), so an engine-only
configure never requires Python.

The gate proves that:

- policy JSON is strict UTF-8 with bounded size/depth/collections, unique keys,
  integer-only numeric fields, exact schemas, and known enum values;
- every declared path is repository-relative and resolves through regular,
  non-reparse, non-symlink components; source/target/seed files cannot be hard links,
  case aliases, or cross-volume paths, and the opened handle is re-checked against the
  root on Windows, Linux, and macOS (any other platform fails closed);
- the 17 declared first-party roots were scanned deterministically within file,
  directory, byte, depth, directory-entry, and shared wall-time limits, with unreadable
  entries fatal and the entry cap applied *before* a directory listing is materialized;
- the declared scope cannot shrink: every `Spark*`/`GameModules` source tree present on
  disk must be a declared root, and the extension set must be the full supported set;
- every detected candidate is either owned by an inventory record or deferred with a
  reason, the `sec-120-parser-triage` owner, the SEC-120 ticket, and an expiry;
- **exclusions are reviewed waivers, not free text.** An exclusion needs an approved
  ticket from a hard-coded allowlist, that ticket's approved owner, an expiry inside
  365 days, and the exact count of candidates it hides. Whole-scan-root, overlapping,
  case-aliased, expired, stale (hiding nothing), and newly-absorbing exclusions are all
  rejected. A single-file exclusion additionally pins a SHA-256 of the reviewed content;
- a parser marked `fuzzed` binds a real build: an `add_executable` that declares the
  target and compiles the declared harness, a listfile reachable from the root
  `CMakeLists.txt` via `add_subdirectory`, `-fsanitize=fuzzer` instrumentation, an
  `add_test` that invokes *that* target and passes the corpus directory plus exact
  `-max_len` / `-timeout` / `-rss_limit_mb` values, and a `set_tests_properties` with a
  matching `TIMEOUT` and a `fuzz` label. **CMake and C++ are tokenized**, so a
  commented-out or string-literal declaration satisfies nothing;
- the harness defines `LLVMFuzzerTestOneInput(const uint8_t*, size_t)` with a body,
  includes a header, defines *and uses* `SPARK_FUZZ_MAX_DEPTH` and
  `SPARK_FUZZ_MAX_INPUT_BYTES` at the budgeted values, compares its input size against
  the input bound, and calls an entry symbol that exists in that parser's own
  inventoried sources;
- corpus seeds are opened and read, not merely `stat`-ed, and a corpus content digest
  pins `last_verified` to exact seed bytes;
- the narrative ledger `docs/sec120-fuzz-policy-evidence.json` cannot claim closure, or
  a non-blocking status, while blockers remain.

The gate does **not** prove that the regex scanner finds every possible parser, or that
declared limits hold at runtime. The snapshot reports `detector_blind_spot_count` — 27
inventoried files that no detector pattern matches, found by human review — precisely so
that limitation is a number rather than an assumption.

## Commands

```bash
# structural gate (blocks merges)
python3 tools/fuzz-policy/check_fuzz_policy.py --source-root . --ci

# closure gate (blocks releases; expected to fail until real targets land)
python3 tools/fuzz-policy/check_fuzz_policy.py --source-root . --ci --require-closure

# regenerate the committed snapshot after changing any policy input
python3 tools/fuzz-policy/check_fuzz_policy.py --source-root . --emit-json \
  > docs/sec120-fuzz-policy-check.json

python3 -m unittest discover -s Tests/fuzz-policy -p "test_*.py" -v
bash tools/check-fuzz-policy.sh          # also runs via tools/validate-all.sh

cmake -S tools/fuzz-policy -B build/fuzz-policy
cmake --build build/fuzz-policy --target check-fuzz-policy
ctest --test-dir build/fuzz-policy --output-on-failure --no-tests=error -C Release
```

`-C Release` is required by multi-config generators (Visual Studio) and ignored by
single-config ones; without it CTest reports `Not Run` on Windows. All validation
failures return nonzero, including JSON emission mode. The CI mode also checks the
workflow/CMake wiring and requires the committed evidence snapshot to equal the
freshly computed report.

## Adding or Reclassifying a Parser

1. Add its production implementation files to
   `tools/fuzz-policy/parser-inventory.json` using canonical repository-relative paths.
2. Mark it `blocked` with a substantive SEC-120 reason, or `fuzzed` with a harness,
   CMake listfile, target, CTest selector, corpus id, and `entry_symbol`.
3. Remove any corresponding entry from `deferred_candidates`.
4. For a fuzzed parser, add exactly one entry to `corpus-manifest.json`. The seed tree
   lives under `Tests/fuzz-corpora/`, must be non-empty, fresh, confined, link-free,
   within every declared limit, and pinned by `content_digest`.
5. Bind the exact input, timeout, memory, depth, and smoke limits in the harness and the
   CMake registration, then run the commands above under ASan/UBSan.
6. Persist each minimized crash input as a regression and regenerate the snapshot.

Do not change a parser to `fuzzed` based on an upstream library campaign or a unit test
that bypasses the production entry point. Adding an exclusion ticket requires editing
`APPROVED_EXCLUSION_TICKETS` in `tools/fuzz-policy/parser_inventory.py` — that code
change *is* the review record.

## Remaining Closure Work

- classify the 149-file deferred backlog before it expires on 2027-02-24;
- implement production-entry-point fuzz targets for the inventoried parsers, starting
  with the highest-risk binary readers (`neural-weights-nnw`, `terrain-sparkterrain`,
  `daemon-asset-cache-blob`, `editor-level-streaming-world`, `startup-splash-bmp`,
  `fps-terrain-heightmap-bmp`, `asset-media-windows`);
- commit bounded seed corpora under `Tests/fuzz-corpora/` and minimized regressions;
- add blocking ASan/UBSan smoke and scheduled campaigns with retained coverage and
  crash-free-duration evidence, and wire the `-L fuzz` smoke run into the CI job (the
  gate already requires it as soon as any parser is marked `fuzzed`);
- independently review that each harness reaches production parsing code and that
  allocation, depth, path, integer, and time bounds are enforced by that code;
- extend the detector so the 27 known blind spots shrink.

## Source & Freshness

Source of truth: `tools/fuzz-policy/`, `cmake/SparkFuzzPolicy.cmake`, the blocking
`fuzz-policy` job in `.github/workflows/build.yml`, and the closure step in
`.github/workflows/release.yml`. Status checked 2026-08-28 against base commit
`006c2ed32f751d3c363e76c3595c78a95069c4bc`; rerun the CI command for current counts.
