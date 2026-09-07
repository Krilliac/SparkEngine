# Fuzz Policy and Parser Security

**Audience:** Engine developers, security reviewers, and CI maintainers
**Thread Context:** SEC-120 — stable-v1 untrusted file and package parsers
**Platform-Backend Scope:** Cross-platform policy tooling; Linux CI is the blocking execution lane

## Current Status

SEC-120 remains open and release-blocking. The repository has a blocking structural
policy gate, but it does not yet have a production fuzz target, seed corpus, sanitizer
fuzz smoke, scheduled campaign, coverage result, or crash-free-duration result.

The deterministic snapshot in `docs/sec120-fuzz-policy-check.json` is validated by CI.
For the recorded source-tree state it reports 46 explicitly inventoried parsers, all
blocked, plus 80 detected candidates awaiting manual classification. Those counts are
not fuzz coverage and must be regenerated whenever the policy inputs change.

Network packet/protocol fuzzing belongs to NET-100 behind G12. AngelScript and
visual-script fuzzing belongs to ENG-200 behind G11. Their source subtrees are named
as ticketed exclusions in the inventory rather than silently omitted.

## What the Gate Proves

The policy tooling lives under the case-sensitive Git path `tools/fuzz-policy/`.
The `fuzz-policy` Linux job runs its standalone CMake project, and Required CI Gate
depends on that job. The root CMake project also exposes `check-fuzz-policy` and the
`FuzzPolicy`/`FuzzPolicyAdversarial` CTest tests.

The gate proves only that:

- policy JSON is strict UTF-8 with bounded size/depth/collections, unique keys,
  integer-only numeric fields, exact schemas, and known enum values;
- every declared path is repository-relative and resolves through regular,
  non-reparse, non-symlink components; source/target/seed files cannot be hard links;
- the twelve declared first-party roots were scanned deterministically within file,
  directory, byte, depth, and wall-time limits, with unreadable entries fatal;
- every detected candidate is either owned by an inventory record or pinned in the
  ticketed classification backlog, so a newly detected candidate fails CI;
- any parser marked `fuzzed` has a one-to-one non-empty corpus and static CMake
  bindings for input length, per-input timeout, memory, smoke timeout, and a harness
  depth constant.

The gate does not prove that a regex scanner finds every possible parser, that a
declared harness reaches the production entry point, or that declared limits hold at
runtime. Those require review plus actual ASan/UBSan fuzz execution.

## Commands

```bash
python3 tools/fuzz-policy/check_fuzz_policy.py --source-root . --ci
python3 -m unittest discover -s Tests/fuzz-policy -p "test_*.py" -v

cmake -S tools/fuzz-policy -B build/fuzz-policy
cmake --build build/fuzz-policy --target check-fuzz-policy
ctest --test-dir build/fuzz-policy --output-on-failure --no-tests=error
```

With a multi-config generator such as Visual Studio, append `-C Debug` (or the
configuration being tested) to the `ctest` command.

All validation failures return nonzero, including JSON emission mode. The CI mode
also checks the workflow/CMake wiring and requires the committed evidence snapshot
to equal the freshly computed report.

## Adding or Reclassifying a Parser

1. Add its production implementation files to
   `tools/fuzz-policy/parser-inventory.json` using canonical repository-relative paths.
2. Mark it `blocked` with a substantive SEC-120 reason, or mark it `fuzzed` with an
   existing harness, CMake file, target, selector, and corpus identifier.
3. Remove any corresponding source from `deferred_candidates`.
4. For a fuzzed parser, add exactly one entry to `corpus-manifest.json`. The seed tree
   must be non-empty, fresh, confined, link-free, and within every declared limit.
5. Bind the exact input, timeout, memory, depth, and smoke limits in the harness and
   CMake registration, then run the commands above under ASan/UBSan.
6. Persist each minimized crash input as a regression and regenerate the evidence
   snapshot.

Do not change a parser to `fuzzed` based on an upstream library campaign or a unit
test that bypasses the production entry point.

## Remaining Closure Work

- classify or explicitly inventory the current candidate backlog;
- implement the required `FuzzSave`, `FuzzScene`, `FuzzAsset`, `FuzzShader`,
  `FuzzArchive`, and `FuzzCrashManifest` production-entry-point targets;
- commit bounded seed corpora and minimized regression inputs;
- add blocking ASan/UBSan smoke and scheduled campaigns with retained coverage and
  crash-free-duration evidence;
- independently review that each harness reaches production parsing code and that
  allocation, depth, path, integer, and time bounds are enforced by that code.

## Source & Freshness

Source of truth: `tools/fuzz-policy/`, `cmake/SparkFuzzPolicy.cmake`, and the blocking
`fuzz-policy` job in `.github/workflows/build.yml`. Status checked 2026-08-28 against
base commit `d34bd6b0885fdaab640726db32ab274fd4e22815`; rerun the CI command for current
counts.
