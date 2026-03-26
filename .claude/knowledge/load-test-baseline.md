# Engine Load Test Baseline (2026-03-26)

**Last updated:** 2026-03-26
**Type:** Observation
**Status:** Active

## Description

Baseline performance numbers from a full engine load test. These serve as regression
benchmarks — future sessions should compare against these numbers to detect performance
regressions before they ship.

## How the Load Test Works

The load test lives in `Tests/TestEngineLoadTest.cpp`. It:

1. **Initializes the full engine** (25+ subsystems via `InitLoadTestEngine()`)
2. **Runs 3,000 heavy frames** — each frame exercises:
   - Physics stepping
   - Weather transitions (every 120 frames)
   - TimeOfDay advancement
   - Coroutine scheduling
   - Tween creation and updates
   - Ability system ticking
   - Instance manager updates
   - 20 entity create/destroy cycles (with Transform + HealthComponent)
   - EventBus publish (1 event per frame, 10 subscribers)
   - NullRHI frame cycle with periodic resource creation
   - GPU perf counter tracking (50 draw calls + 5,000 primitives/frame)
   - Profiler frame cycle
   - JobSystem ParallelFor (500 items every 10 frames)
3. **Samples OS resources** every 50 frames via `/proc/self/status` + `getrusage()`
4. **Reports** frame timing percentiles, CPU user/system/total, memory RSS/VSZ

## How to Run It Yourself

```bash
# Build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build --config Release --parallel $(nproc)

# Run just the load test (grep for output)
./build/bin/SparkTests 2>&1 | grep -A 50 "FULL ENGINE LOAD TEST"

# Or run all tests
./build/bin/SparkTests
```

The load test is `TEST(LoadTest_FullEngine_3000Frames)` in `Tests/TestEngineLoadTest.cpp`.

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

## Regression Thresholds

If a future load test shows any of these, investigate before merging:

- **Avg frame time > 50 us** (currently 8.7 us — 5.7x headroom)
- **P99 frame time > 500 us** (currently 62.5 us — 8x headroom)
- **RSS growth > 1 MB** over 3,000 frames (currently 20 KB)
- **RSS peak > 50 MB** (currently 35.2 MB)
- **CPU utilization < 100%** would indicate JobSystem threads aren't being used
- **Events delivered != frame count** would indicate EventBus type mismatch or subscription leak

## Adverse Condition Results (2026-03-26)

These test resilience under non-ideal but plausible runtime conditions.

### 10,000 Entities Alive Simultaneously

| Metric | Value |
|--------|-------|
| Create 10k entities (with Transform + Health + Light) | 49.1 ms |
| Destroy 10k entities | 24.9 ms |
| Frame avg while 10k alive (100 frames) | 0.6 us |
| RSS after create | 37.0 MB |
| RSS after destroy | 37.0 MB |

### Thread Contention (50 batches x 200 jobs, tight atomic contention)

| Metric | Value |
|--------|-------|
| Total jobs completed | 10,000/10,000 |
| Batch avg | 400 us |
| Batch min | 320 us |
| Batch max | 629 us |

### State Thrashing (weather + time + tween + coroutine change every frame)

| Metric | Value |
|--------|-------|
| Frame avg (2000 frames) | 0.4 us |
| Frame max | 6.0 us |
| Memory delta | 0 KB |

## Severe Condition Results (2026-03-26)

These push subsystems to breaking point to find limits.

### 100,000 Entity Flood

| Metric | Value |
|--------|-------|
| Create 100k (Transform only) | 1,944 ms (19.4 us/entity) |
| Destroy 100k | 2,341 ms (23.4 us/entity) |
| RSS before | 37.0 MB |
| RSS peak | 46.7 MB |
| RSS after destroy | 46.7 MB (pools retained) |

### EventBus Storm (100k events x 50 subscribers = 5M deliveries)

| Metric | Value |
|--------|-------|
| Total deliveries | 5,000,000 / 5,000,000 |
| Total time | 78.1 ms |
| Per event (50 subscribers) | 0.78 us |
| Throughput | **64 million deliveries/sec** |

### JobSystem Flood (10,000 async jobs with real work)

| Metric | Value |
|--------|-------|
| Completed | 10,000 / 10,000 |
| Total time | 20.3 ms |
| Per job | 2.03 us |
| Throughput | **493k jobs/sec** |

### Save/Load Cycling (50 rapid save/load cycles, 100 entities)

| Metric | Value |
|--------|-------|
| Save avg | 766 us |
| Save max | 1,142 us |
| Load avg | 530 us |
| Load max | 842 us |

### Network Server Churn (50 rapid start/stop cycles)

| Metric | Value |
|--------|-------|
| Successful | 50/50 |
| Total time | 2.6 ms |
| Per cycle | 52 us |

## Notes

- The `>10x avg` spikes (12 out of 3000) are caused by JobSystem thread wake-up latency
  on the first ParallelFor dispatch of a batch. This is expected OS scheduling behavior.
- RSS stays flat because entity churn (create/destroy every frame) reuses EnTT's recycled
  entity slots and component pools. The 20 KB growth is likely allocator metadata.
- CPU utilization > 100% confirms the 4-thread JobSystem is doing real parallel work
  during ParallelFor dispatches.
- The NullRHI device adds no overhead — it's purely tracking counters, not touching GPU.
