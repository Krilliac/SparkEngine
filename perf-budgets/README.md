# Performance Budget Governance (PERF-100)

Versioned performance budgets and golden-baseline governance for
SparkEngine release gates.

## Directory layout

```
perf-budgets/
  README.md              ← this file
  v1/
    hardware.json        ← certified hardware rows
    budget.json          ← metric definitions with budget thresholds
    baselines.json       ← approved golden baselines (approval-governed)
    baselines/           ← future: per-scene golden-image baselines
```

## Schema version

All files carry the exact supported `schemaVersion` (`1.0.0`). The validator
rejects unknown keys at every level, wrong value types, duplicate JSON object
keys, unsupported schema versions, non-finite numbers, oversized documents,
and excessive row/metric/result counts. Adding a field or version requires a
corresponding validator and regression-test update.

`budget.metadata.commitSha` is nullable by design: a tracked file cannot
truthfully contain the hash of the commit that contains that same file. Exact
run provenance instead comes from the workflow's externally supplied
`--expected-sha`, which must match the full SHA inside the result document.

## Metric definitions

Each metric in `budget.json` has:

| Field | Purpose |
|-------|---------|
| `id` | Unique dotted identifier (e.g. `d3d11.client.frame_time.p50`) |
| `category` | One of: `frame_time`, `tick_time`, `startup_time`, `memory`, `package_size`, `soak`, `visual`, `throughput` |
| `unit` | Explicit unit: `ms`, `us`, `ns`, `s`, `bytes`, `kilobytes`, `megabytes`, `gigabytes`, `bytes_per_hour`, `count`, `percent`, `fps` |
| `direction` | `lower_is_better` or `higher_is_better` |
| `status` | `pending_measurement` (no budget yet), `active`, `suspended`, `retired` |
| `budget` | Numeric threshold or `null` if pending |
| `hardwareRowId` | Reference to a certified hardware row, or `null` for hardware-independent metrics |

Status behavior is explicit:

- `active` requires a numeric budget and is enforced only on its declared
  hardware row (or on the current row when `hardwareRowId` is `null`).
- `pending_measurement` requires `budget: null` and is reported as skipped.
- `suspended` may retain its former budget for review, but is always reported
  as skipped and never blocks completeness.
- `retired` requires `budget: null` and is always reported as skipped.

## Hardware certification

Hardware rows in `hardware.json` start as `certified: false`.
Certification requires:

1. Run the full benchmark suite on the hardware
2. Record `certifiedAt` (ISO-8601), `certifiedBy` (reviewer name),
   and `certifiedCommit` (the commit SHA where certification occurred)
3. Set `certified: true`

Results from uncertified hardware are flagged as advisory and do not turn an
otherwise clean comparison into a regression. They are marked
`authoritative: false` and cannot be used as release evidence.

## Baseline approval governance

Baseline entries in `baselines.json` are distinct from ordinary metric
edits.  Every baseline requires:

- `approvedBy` — the reviewer who approved the baseline value
- `approvedAt` — ISO-8601 timestamp of approval
- `approvalCommit` — the commit SHA where approval was recorded

Self-approval (where `approvalCommit == commitSha`) is rejected by
default.  The `approvalPolicy.selfApprovalAllowed` flag can override
this for development/bootstrap, but production releases must use
independent review.

The policy object is exact and `selfApprovalAllowed` must be a JSON boolean;
strings such as `"false"` are rejected. Baseline timestamps must be real,
timezone-aware RFC3339 values, and approval cannot precede measurement.
Baseline metric, unit, and hardware references are checked against the active
budget and hardware schemas both in standalone validation and in comparison.

## Validation

```bash
python3 tools/perf-budget/validate_budget.py perf-budgets/v1
```

The validator is fail-closed:
- Missing required fields → error
- Unknown/extra fields → error
- Duplicate IDs → error
- Invalid enum values → error
- Hardware references to non-existent rows → error
- Active metrics with null budgets → error
- Malformed JSON → error
- Duplicate JSON object keys → error
- Files larger than 2 MiB or excessive collection counts → error
- Impossible or timezone-free timestamps → error
- Unknown nested fields and wrong JSON types → error

## Comparing results

```bash
python3 tools/perf-budget/compare_results.py perf-budgets/v1 results.json \
  --expected-sha "$GITHUB_SHA"
python3 tools/perf-budget/compare_results.py perf-budgets/v1 results.json \
  --expected-sha "$GITHUB_SHA" --json
```

The comparator:
- Validates the result file structurally before any comparison
- Rejects unit mismatches between result and budget
- Rejects measurements for unknown metrics
- Flags results from uncertified hardware
- Reports pass/fail per metric with margin percentages
- Binds every verdict to the exact commit SHA and hardware row
- Requires a full 40- or 64-character SHA supplied independently by the
  workflow and rejects a result whose self-asserted SHA does not match
- Loads and validates `baselines.json`; a missing or invalid approval policy
  fails the comparison before any budget can pass
- Requires complete measurements only for active metrics applicable to the
  result's current hardware row
- Emits `margin_percent: null` plus `margin_reason` when a zero budget makes a
  percentage denominator undefined
- Reports every pending, suspended, or retired measurement with an explicit
  status and reason

### Result file format

```json
{
  "commitSha": "0123456789abcdef0123456789abcdef01234567",
  "timestamp": "2026-08-28T12:00:00Z",
  "hardwareRowId": "windows-d3d11-dev-primary",
  "measurements": [
    {
      "metricId": "d3d11.client.frame_time.p50",
      "value": 12.5,
      "unit": "ms",
      "sampleCount": 1000
    }
  ]
}
```

## How later measurements plug in

### D3D11 frame budgets
1. Create benchmark scenes using `IBenchmarkScenario` from `Utils/BenchmarkFramework.h`
2. Run on certified hardware, capture p50/p95/p99 frame times
3. Set `budget` values in `budget.json`, change `status` to `active`
4. Add a `performance-regression` CI job that runs `compare_results.py`

### NullRHI headless tick budgets
1. Run headless tick loop via `--nullrhi` flag
2. Capture tick-time percentiles on the CI runner
3. Populate `nullrhi.headless.tick_time.*` budgets

### Startup time
1. Instrument `EngineContext::Initialize()` with wall-clock timing
2. Capture cold-start times for client and editor
3. Populate `windows.*.startup_time` budgets

### Memory budgets
1. Use `MemoryDebugger.h` peak-tracking APIs
2. Capture peak RSS and VRAM during benchmark scenes
3. Populate `*.memory.peak_rss` and `*.memory.peak_vram` budgets

### Package size
1. Run full Release build and packaging
2. Measure binary and packaged sizes
3. Populate `*.package_size` budgets

### Soak tests
1. Run NullRHI headless loop for 1+ hours
2. Monitor for leaks (MemoryDebugger), crashes, and deadlocks
3. Populate `nullrhi.soak.*` budgets (leak_rate=0, crash_count=0)

### Visual regression (golden images)
1. Use existing `GoldenImageTestRunner` from `Utils/GoldenImageTest.h`
2. Capture reference frames on certified hardware
3. Commit to `Tests/GoldenImages/` with baseline approval
4. Add `golden-d3d11` CI job

### CI integration

The blocking `performance-budget-governance` job in
`.github/workflows/build.yml` runs the full Python governance suite and
validates the committed schemas on every Build workflow push and pull request.
That workflow intentionally has no path filter, so changes under
`perf-budgets/**`, `tools/perf-budget/**`, the relevant tests, or the workflow
itself cannot bypass the required gate.

The actual benchmark scenes, populated thresholds, certified baselines,
golden-image comparison, long soaks, and release-result production remain open
PERF-100 work. Those later jobs must supply `${{ github.sha }}` through
`--expected-sha` and upload the result/report as exact-commit evidence.

## Tests

```bash
python3 -m unittest \
  Tests.Tools.test_perf_budget \
  Tests.Tools.test_perf_budget_adversarial \
  Tests.Tools.test_perf_budget_hardening -v
```

As of the PERF-100 hardening slice, this command runs 137 test methods. The
count includes one named regression for each of the 21 independently
reproduced second-audit defects; it does not treat subtests as separate test
methods.
