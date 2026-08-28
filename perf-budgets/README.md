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

All files carry a `schemaVersion` field.  The validator rejects unknown
keys — adding a new field requires a schema-version bump and a
corresponding validator update.

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

## Hardware certification

Hardware rows in `hardware.json` start as `certified: false`.
Certification requires:

1. Run the full benchmark suite on the hardware
2. Record `certifiedAt` (ISO-8601), `certifiedBy` (reviewer name),
   and `certifiedCommit` (the commit SHA where certification occurred)
3. Set `certified: true`

Results from uncertified hardware are flagged as advisory — they do not
block releases.

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

## Comparing results

```bash
python3 tools/perf-budget/compare_results.py perf-budgets/v1 results.json
python3 tools/perf-budget/compare_results.py perf-budgets/v1 results.json --json
```

The comparator:
- Validates the result file structurally before any comparison
- Rejects unit mismatches between result and budget
- Rejects measurements for unknown metrics
- Flags results from uncertified hardware
- Reports pass/fail per metric with margin percentages
- Binds every verdict to the exact commit SHA and hardware row

### Result file format

```json
{
  "commitSha": "abc1234...",
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
A future `.github/workflows/perf-regression.yml` will:
1. Run benchmark scenarios tagged `Benchmark_*`
2. Produce a `results.json` in the format above
3. Call `compare_results.py` to gate the PR
4. Upload the results JSON as a workflow artifact

## Tests

```bash
python3 -m pytest Tests/Tools/test_perf_budget.py -v
# or
python3 -m unittest Tests.Tools.test_perf_budget -v
```
