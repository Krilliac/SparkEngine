# Fuzz Policy and Parser Security

**Audience:** Engine developers, security reviewers, and CI maintainers
**Thread Context:** SEC-120 — stable-v1 untrusted file and package parsers
**Platform-Backend Scope:** Cross-platform policy tooling; Linux CI is the blocking execution lane

## Current Status

SEC-120 remains open and release-blocking. The repository now has five structurally
validated production fuzz targets and bounded seed corpora for `json-utils`,
`crash-manifest-parser`, `texture-stex-compressor`, `neural-weights-nnw`, and
`scene-manifest`, but
exact-SHA hosted sanitizer evidence, scheduled campaigns, coverage, and
crash-free-duration evidence remain absent.

The deterministic snapshot in `docs/sec120-fuzz-policy-check.json` is validated by CI.
For the recorded source-tree state it reports **135 explicitly inventoried parsers, 5
fuzzed and 130 blocked**, **5 bound corpora with 37 seeds (30857 bytes)**, **0 deferred
candidates and 118 OD-21 exemptions**, and **2006 source files scanned across 17
first-party roots**. Those counts are not fuzz coverage.
`passed` in that snapshot is computed from the closure blockers, so it reads `false`
while any blocker remains.

The neural CTest uses `-runs=8` to replay all eight reviewed seeds, and the crash-manifest
CTest replays its six reviewed seeds, under ASan/UBSan without mutating the tracked
corpus. The texture-stex and scene-manifest CTests replay their eight reviewed seeds the
same way, and the json-utils CTest replays its seven (`-runs=7`). The json-utils smoke
previously mutated for `-max_total_time=4` with no `-runs`, so its execution count
varied run to run (about 300,000) and it wrote several hundred mutated units into the
tracked `Tests/fuzz-corpora/json-utils/` directory. Every smoke now runs only the empty
input plus its reviewed seeds. libFuzzer's leak check can still run one seed a second
time when malloc/free counts differ, so the `Done N runs` line may read one higher. The
scene-manifest adapter aborts when an accepted asset path climbs out of the
root under Windows separator semantics (its own lexical walk splits on both `/` and `\`,
so it does not merely re-run the parser's filter), contains a control byte such as an
embedded NUL, or fails `IsVirtualPathSafe`, or when the entry cap is exceeded. The
production `IsVirtualPathSafe` itself treats `\` as a separator on every host, so the
Linux-hosted target covers Windows separator semantics as well as POSIX ones
(`SceneManifest_ParseDropsBackslashTraversalOnEveryHost`); other Win32 name quirks such
as 8.3 short names are not modelled. The 8 MB and 100,000-entry caps sit above its
64 KiB `-max_len`, so the `SceneManifest_EntryCap*`/`SceneManifest_ByteCap*` unit tests pin those boundaries. These
are seed-smoke checks, not mutation campaigns; the scheduled campaign (below) mutates a
disposable writable corpus and retains its results. Mutation runs on the larger scene-manifest seeds
trip `-rss_limit_mb=256` through ASan's default 256 MB quarantine alone, so run campaigns
with `ASAN_OPTIONS=quarantine_size_mb=32` (a local 180-second campaign then completed
86,134 executions with no finding).

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
- every detected candidate is owned by an inventory record, deferred with a reason, the
  `sec-120-parser-triage` owner, the SEC-120 ticket, and an expiry, or carries an OD-21
  exemption (see [OD-21 Candidate Classification](#od-21-candidate-classification));
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
declared limits hold at runtime. The full inventory report records 28 inventoried files
that no detector pattern matches, found by human review, so that limitation is a number
rather than an assumption.

## OD-21 Candidate Classification

Owner decision OD-21 fixes how a detected candidate is classified, one entry per file,
and fails closed toward fuzzing:

1. Code that parses bytes from outside the process — files a user or mod can supply,
   packages, network, save/scene/config files read at runtime — is an inventoried
   parser in `parsers[]` and needs a fuzz target. `trust_boundary` is one of
   `untrusted-file`, `untrusted-network`, or `untrusted-ipc` (pipes, child-process
   output and health files, git output). Until a harness exists it is `blocked`
   with a SEC-120 blocker; it is never exempted.
2. Code that only parses data this same process produced, or build-time/developer
   tooling that never ships, is exempt with a written justification.
3. Generic read/tokenize helpers are helper-exempt; every parser that calls them is
   classified on its own.

When in doubt, the file is a boundary. Exemptions live in `exempt_candidates[]`:

| `classification` | Meaning |
|---|---|
| `same-process-data` | rule (2): decodes only bytes this process generated (e.g. the in-memory play-mode snapshot) |
| `developer-tooling` | rule (2): build/test tooling no shipped runtime path reaches |
| `helper` | rule (3): generic read/tokenize/hash helper with no grammar of its own |
| `delegating-call-site` | forwards to, or only declares, an inventoried parser's entry point; `delegates_to` must name that parser id or an excluded subtree |
| `not-a-parser` | the detector matched code that decodes no externally suppliable bytes (doc comments, atomic ring indices, kernel-owned `/proc` reads, display-only log tails) |

Each exemption also records `detected_by`, the exact detector hits the reviewer read.
The gate requires the live scan to report the same set, so adding a new kind of parsing
to an exempt file fails CI until the file is re-reviewed. Stale, duplicate, case-aliased,
unjustified (under 40 characters), and parser-or-deferral-overlapping exemptions are
rejected. The first triage (2026-09-24) classified all 149 deferred candidates: 28 new
blocked boundaries (including the collaborative-edit TCP codec, the editor/engine named
pipe, LAN discovery beacons, game-module save-state decoders, and two tinyobj-based OBJ
loaders), 3 files folded into existing blocked records, and 115 exemptions.

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

# Ubuntu/Debian's compiler-rt libFuzzer archive uses libstdc++. The fuzzer
# executable and callback stay on that ABI, while the production adapter and
# logger compile with libc++ for the C++23 headers. They communicate only via
# an extern "C" byte-buffer entry point, so no C++ standard-library object
# crosses the boundary.
sudo apt-get install -y clang cmake libc++-dev libc++abi-dev
CXX=clang++ CXXFLAGS="-stdlib=libstdc++" \
  LDFLAGS="-stdlib=libstdc++" \
  cmake -S tools/fuzz-policy -B build/fuzz-policy
cmake --build build/fuzz-policy --target check-fuzz-policy
cmake --build build/fuzz-policy --target SparkFuzzJsonUtils SparkFuzzCrashManifest SparkFuzzNeuralWeights SparkFuzzTextureStex SparkFuzzSceneManifest
ctest --test-dir build/fuzz-policy --output-on-failure --no-tests=error -C Release
ctest --test-dir build/fuzz-policy --output-on-failure -L '^fuzz$' --no-tests=error -C Release
```

`-C Release` is required by multi-config generators (Visual Studio) and ignored by
single-config ones; without it CTest reports `Not Run` on Windows. All validation
failures return nonzero, including JSON emission mode. The CI mode also checks the
workflow/CMake wiring and requires the committed evidence snapshot to equal the
freshly computed report.

## Scheduled Campaign

`.github/workflows/fuzz-scheduled.yml` (job `fuzz-scheduled`, nightly and
`workflow_dispatch`) is the exploration half of the gate. It is deliberately not a need
of `required-ci-gate`: a campaign finding is a new bug to fix with a regression seed,
not a reason to block unrelated merges. `tools/fuzz-policy/run_campaign.py` discovers
every CTest test labelled exactly `fuzz` in the configured build, so a new target joins
the campaign when its smoke is registered. For each target it:

- copies the committed corpus into a temporary directory and gives libFuzzer only that
  copy, then re-hashes the committed corpus and reports `corpus-mutated` if it changed
  (the workflow also fails on any `git status` change under `Tests/fuzz-corpora`);
- keeps the smoke's `-max_len`/`-timeout`/`-rss_limit_mb`, drops its replay-only
  `-runs`/`-max_total_time`, and mutates for `--seconds` (600 per target by default);
- caps ASan's quarantine at 32 MB unless `ASAN_OPTIONS` already sets one, so the 256 MB
  RSS limit does not report false OOMs;
- sets `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` unless `halt_on_error` is
  already set, so undefined behaviour in a target built with recoverable UBSan
  (`SparkFuzzJsonUtils` lacks `-fno-sanitize-recover=undefined`) aborts and leaves a
  `crash-` reproducer, and reports `sanitizer-report` if a `runtime error:` line is in
  the log of an otherwise clean run;
- runs `-minimize_crash=1` on every `crash-`/`leak-`/`timeout-`/`oom-` reproducer and
  keeps both the raw and the minimized file;
- rewrites `campaign-summary.json` after every target (`complete` stays `false` until
  the last one finishes, so a cancelled job still uploads partial results) with
  per-target status, duration, executed units, crash-free wall time, peak RSS, new
  units and artifact SHA-256s.

It exits 1 on any finding (crash, hang, abnormal exit, UBSan report or corpus change).
Without a finding it exits 2 when the campaign could not be set up (bad arguments, no
targets, a budget above `--max-campaign-seconds`) or when a target could not run
(non-executable binary or unusable corpus, recorded as `setup-error` in the summary).
The workflow passes `--max-campaign-seconds 6000`, so `seconds_per_target` times the
discovered target count must fit 100 of the job's 180 minutes.
The workflow uploads the whole output directory as `fuzz-campaign-<run id>` for 90
days. To land a finding, reproduce with the minimized file, fix the parser, and commit
that file as a `regression-*` seed with an updated `corpus-manifest.json` digest.

```bash
python3 tools/fuzz-policy/run_campaign.py --build-dir build/fuzz-policy \
  --output /tmp/fuzz-campaign --seconds 30
git status --porcelain -- Tests/fuzz-corpora   # must print nothing
```

A workflow file proves nothing until a hosted run is recorded; no scheduled-campaign
history exists yet, so `runtime_evidence.scheduled_campaign` stays `false`.

## Adding or Reclassifying a Parser

1. Add its production implementation files to
   `tools/fuzz-policy/parser-inventory.json` using canonical repository-relative paths.
2. Mark it `blocked` with a substantive SEC-120 reason, or `fuzzed` with a harness,
   CMake listfile, target, CTest selector, corpus id, and `entry_symbol`. If the
   harness crosses a C ABI adapter, also declare its `binding_source` and
   `harness_entry_symbol`; the adapter must call the production `entry_symbol`.
3. Remove any corresponding entry from `deferred_candidates` or `exempt_candidates`.
   A newly detected file that is not a boundary gets an OD-21 exemption instead, with
   its `classification`, a concrete `justification`, and the sorted `detected_by`
   list the scanner reports for it.
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

- retain exact-SHA sanitizer smoke for json-utils, crash-manifest-parser,
  texture-stex-compressor, neural-weights-nnw, and scene-manifest, then implement production
  entry-point fuzz targets for the remaining 130 blocked parsers, starting
  with the highest-risk binary readers (`terrain-sparkterrain`,
  `daemon-asset-cache-blob`, `editor-level-streaming-world`, `startup-splash-bmp`,
  `fps-terrain-heightmap-bmp`, `asset-media-windows`);
- commit bounded seed corpora under `Tests/fuzz-corpora/` and minimized regressions;
- retain the blocking ASan/UBSan smoke now wired for both targets and record hosted
  `fuzz-scheduled` campaign history with its crash-free-duration statistics, then add
  coverage reporting (the `-L fuzz` CI run is required whenever a parser is marked
  `fuzzed`);
- independently review that each harness reaches production parsing code and that
  allocation, depth, path, integer, and time bounds are enforced by that code;
- extend the detector so the 28 known blind spots shrink;
- have an independent security reviewer re-check the 115 OD-21 exemptions; they are
  recorded judgement, not proof of unreachability.

## Source & Freshness

Source of truth: `tools/fuzz-policy/`, `cmake/SparkFuzzPolicy.cmake`, the blocking
`fuzz-policy` job in `.github/workflows/build.yml`, the non-blocking `fuzz-scheduled`
campaign in `.github/workflows/fuzz-scheduled.yml`, and the closure step in
`.github/workflows/release.yml`. The OD-21 classification and the counts above were
re-verified structurally 2026-09-25 on the release worktree; rerun the CI command for
current counts and exact-SHA runtime evidence.
