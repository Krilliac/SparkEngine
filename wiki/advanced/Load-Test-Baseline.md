# Load Test Baseline

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (audit/reference) — load test exercises the full multi-threaded engine (Physics, JobSystem, EventBus, ECS)
>
> **Platform/Backend Scope:** Linux x86_64, GCC Release, NullRHI (headless) — numbers are platform-specific

## Overview

Baseline performance numbers from a full engine load test. These serve as regression benchmarks — future runs should compare against them to catch performance regressions before they ship. The baseline below was captured 2026-03-26 on Linux x86_64 / GCC Release. The test harness (`Tests/TestEngineLoadTest.cpp`, 1,716 lines) and all of its scenarios are confirmed present as of 2026-06-08; baseline numbers should be re-captured on a controlled machine before being treated as a hard gate.

---

## How the Load Test Works

The load test lives in `Tests/TestEngineLoadTest.cpp`. It:

1. **Initializes the full engine** (25+ subsystems via `InitLoadTestEngine()`).
2. **Runs 3,000 heavy frames**, each exercising: physics stepping, weather transitions (every 120 frames), TimeOfDay advancement, coroutine scheduling, tween updates, ability ticking, instance manager updates, 20 entity create/destroy cycles (Transform + HealthComponent), EventBus publish (1 event/frame, 10 subscribers), a NullRHI frame cycle with periodic resource creation, GPU perf-counter tracking (50 draw calls + 5,000 primitives/frame), a profiler frame cycle, and a JobSystem `ParallelFor` (500 items every 10 frames).
3. **Samples OS resources** every 50 frames via `/proc/self/status` + `getrusage()`.
4. **Reports** frame-timing percentiles, CPU user/system/total, and memory RSS/VSZ.

### Scenarios present (verified 2026-06-08)

`LoadTest_FullEngine_3000Frames`, `LoadTest_Adverse_HighEntityCount`, `LoadTest_Adverse_ThreadContention`, `LoadTest_Adverse_StateThrashing`, `LoadTest_Severe_EntityFlood`, `LoadTest_Severe_EventBusStorm`, `LoadTest_Severe_JobSystemFlood` (plus save/load and network-churn scenarios described below).

---

## How to Run It

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --config Release --parallel $(nproc)

# Just the load test output:
./build/bin/SparkTests 2>&1 | grep -A 50 "FULL ENGINE LOAD TEST"
```

---

## Baseline Numbers (2026-03-26, Linux x86_64, GCC Release)

### Frame Timing

| Metric | Value |
|--------|-------|
| Min | 3.5 us |
| Avg | 8.7 us |
| P50 | 3.8 us |
| P95 | 38.7 us |
| P99 | 62.5 us |
| Max | 141.9 us |
| Total (3000 frames) | 27.0 ms |
| Effective FPS | 110,978 |
| Spikes (>3x avg) | 305 |
| Spikes (>10x avg) | 12 |

### CPU Usage

| Metric | Value |
|--------|-------|
| User time | 20.0 ms |
| System time | 13.2 ms |
| Total CPU | 33.2 ms |
| Utilization | 122.8% (multi-threaded) |

### Memory (RSS)

| Metric | Value |
|--------|-------|
| Min | 35.1 MB |
| Avg | 35.2 MB |
| Max | 35.2 MB |
| Growth | 20 KB |
| VSZ Max | 1165.9 MB |

### Workload

| Metric | Value |
|--------|-------|
| Entities created/destroyed | 60,000 |
| EventBus events delivered | 3,000 |
| ParallelFor batches | 300 (150,000 work items) |
| Weather transitions | 26 |

---

## Regression Thresholds

If a future run shows any of these, investigate before merging:

- **Avg frame time > 50 us** (baseline 8.7 us — 5.7x headroom)
- **P99 frame time > 500 us** (baseline 62.5 us — 8x headroom)
- **RSS growth > 1 MB** over 3,000 frames (baseline 20 KB)
- **RSS peak > 50 MB** (baseline 35.2 MB)
- **CPU utilization < 100%** would indicate JobSystem threads aren't being used
- **Events delivered != frame count** would indicate an EventBus type mismatch or subscription leak

---

## Adverse & Severe Condition Results (2026-03-26)

### 10,000 Entities Alive Simultaneously

| Metric | Value |
|--------|-------|
| Create 10k (Transform + Health + Light) | 49.1 ms |
| Destroy 10k | 24.9 ms |
| Frame avg while 10k alive (100 frames) | 0.6 us |
| RSS after create / destroy | 37.0 MB / 37.0 MB |

### Thread Contention (50 batches × 200 jobs, tight atomic contention)

| Metric | Value |
|--------|-------|
| Jobs completed | 10,000/10,000 |
| Batch avg / min / max | 400 us / 320 us / 629 us |

### State Thrashing (weather + time + tween + coroutine changed every frame)

| Metric | Value |
|--------|-------|
| Frame avg (2000 frames) / max | 0.4 us / 6.0 us |
| Memory delta | 0 KB |

### 100,000 Entity Flood

| Metric | Value |
|--------|-------|
| Create 100k (Transform only) | 1,944 ms (19.4 us/entity) |
| Destroy 100k | 2,341 ms (23.4 us/entity) |
| RSS before / peak / after | 37.0 MB / 46.7 MB / 46.7 MB (pools retained) |

### EventBus Storm (100k events × 50 subscribers = 5M deliveries)

| Metric | Value |
|--------|-------|
| Total deliveries | 5,000,000 / 5,000,000 |
| Total time | 78.1 ms |
| Throughput | **64 million deliveries/sec** |

### JobSystem Flood (10,000 async jobs with real work)

| Metric | Value |
|--------|-------|
| Completed | 10,000 / 10,000 |
| Total time / per job | 20.3 ms / 2.03 us |
| Throughput | **493k jobs/sec** |

### Save/Load Cycling (50 cycles, 100 entities) & Network Server Churn (50 start/stop)

| Metric | Value |
|--------|-------|
| Save avg / max | 766 us / 1,142 us |
| Load avg / max | 530 us / 842 us |
| Network cycles successful | 50/50 (2.6 ms total, 52 us/cycle) |

---

## Notes

- The `>10x avg` spikes (12 of 3,000) are JobSystem thread wake-up latency on the first `ParallelFor` dispatch of a batch — expected OS scheduling behavior.
- RSS stays flat because per-frame entity churn reuses EnTT's recycled entity slots and component pools; the 20 KB growth is likely allocator metadata.
- CPU utilization > 100% confirms the 4-thread JobSystem is doing real parallel work.
- NullRHI adds no overhead — it only tracks counters, never touching a GPU.

---

## Source & Freshness

- **Original baseline:** `.claude/knowledge/load-test-baseline.md`, dated 2026-03-26.
- **Re-measured against codebase 2026-06-08** (harness presence verified; baseline numbers carried forward, not re-run).
- OLD → NEW notes:
  - `Tests/TestEngineLoadTest.cpp` confirmed present and is now **1,716 lines**.
  - All listed scenarios verified present in the current file (full-engine, adverse, severe, save/load, network churn).
  - Frame-timing, CPU, memory, and stress numbers are unchanged from the 2026-03-26 capture — they are hardware-specific and should be re-captured on a controlled machine before being enforced as a CI gate.
- Findings now resolved/changed: none — the test exists and matches its documented structure.

## Related Pages

- [Benchmark Framework](Benchmark-Framework.md)
- [Performance Profiling Guide](Performance-Profiling-Guide.md)
- [Performance Tips](Performance-Tips.md)
- [Threading Model](Threading-Model.md)
- [Test Suite Audit](Test-Suite-Audit.md)
