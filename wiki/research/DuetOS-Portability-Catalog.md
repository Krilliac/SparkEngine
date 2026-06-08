# DuetOS Portability Catalog

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** Cross-project reference

## Overview

[DuetOS](https://github.com/) (a sibling project: a from-scratch C++23/Rust x86_64 operating system with a Win32/PE subsystem) shares an author and a coding lineage with SparkEngine. This page catalogs DuetOS components that were evaluated for reuse in SparkEngine, with an explicit **why / why-not** verdict for each.

**Headline conclusion:** Very little should be *literally* ported. DuetOS code is **freestanding kernel code** — it deliberately avoids the C++ standard library, libc, exceptions, and the heap-until-late, and reimplements primitives that SparkEngine already gets for free from `std::` and its existing mature subsystems (EnTT, Jolt, the engine `JobSystem`, `Logger`, `Profiler`, `SPARK_HAS_EXPECTED`/`std::expected`). The real value DuetOS offers SparkEngine is **architectural reference** (its wiki/docs discipline and a couple of ergonomic idioms), not drop-in modules.

This catalog exists so the evaluation is recorded and we don't re-litigate it each time someone notices the overlap.

## How to Read This

Each entry lists the DuetOS source location, what it does, and a verdict:

- **Adopt** — genuinely additive; worth porting/adapting.
- **Reference only** — study the design, but SparkEngine should use its own/`std::` version.
- **Skip** — kernel-specific or fully duplicative; no action.

## Idioms & Utilities

| Component | DuetOS path | What it is | Verdict | Why / why-not |
|-----------|-------------|------------|---------|---------------|
| `Result<T,E>` | `kernel/util/result.h` | Exception-free result type with `RESULT_TRY` macros | **Skip** | SparkEngine already standardizes on C++23 `std::expected` via `SPARK_HAS_EXPECTED` (see `Core/Platform.h`). Introducing a second result type would fragment error handling and create conversion friction. |
| `ScopeGuard` / `DUETOS_DEFER` | `kernel/util/defer.h` | RAII scope-exit cleanup (Go-style `defer`) | **Reference only** | A legitimately nice ergonomic. But it's ~20 lines and trivially re-expressible; if the engine wants `defer`, write a small `Spark::ScopeGuard` in `Utils/` rather than importing DuetOS's. Low priority — RAII wrappers already cover most cases. |
| Saturating arithmetic | `kernel/util/saturating.h` | Clamp-instead-of-wrap add/sub/mul | **Skip** | Niche; `std::` + manual clamps suffice where needed. No current call site demand. |
| String ops (`MemcpyChecked`, etc.) | `kernel/util/string.h` | Freestanding, bounds-checked mem/str ops | **Skip** | Exists only because the kernel has no libc. SparkEngine has full `<cstring>`/`std::string`/`std::span`. |
| Unicode width/case-fold | `kernel/util/unicode.h` | UTF-8 helpers | **Skip** | The editor/UI already handle text via ImGui + existing localization; no gap. |

## Memory & Allocators

| Component | DuetOS path | What it is | Verdict | Why / why-not |
|-----------|-------------|------------|---------|---------------|
| Slab / object-pool allocator | `kernel/mm/slab.h` | Fixed-size object cache, O(1) alloc/free | **Reference only** | The *idea* (per-type pools for particles, physics bodies, audio voices) is sound, but the kernel slab is coupled to the kernel page allocator. If profiling shows allocator pressure in a hot pool, write an engine-native `Spark::ObjectPool<T>` over `std::pmr` rather than port kernel code. No current measured need. |
| KHeap (coalescing freelist) | `kernel/mm/kheap.h` | General-purpose heap | **Skip** | SparkEngine uses the platform allocator + `std::pmr`; replacing the global allocator is out of scope and risky. |
| Frame/zone allocators | `kernel/mm/frame_allocator.h`, `zone.cpp` | Physical-page / DMA-zone allocators | **Skip** | Kernel-only concepts (physical memory, IOMMU zones). Not applicable. |
| Poison allocator | `kernel/mm/poison_alloc.h` | Fills freed memory with sentinels | **Skip** | ASan/UBSan (already in CI) cover use-after-free far better on the engine's platforms. |

## Concurrency

| Component | DuetOS path | What it is | Verdict | Why / why-not |
|-----------|-------------|------------|---------|---------------|
| Spinlock / adaptive mutex / rwlock / seqlock | `kernel/sync/*` | Hand-rolled sync primitives with IRQ save/restore | **Skip** | These exist because a kernel can't call `std::mutex`. SparkEngine has `std::mutex`/`std::shared_mutex`/`std::atomic` and documented thread-safety rules per subsystem. Hand-rolled spinlocks in user space are usually a pessimization. |
| RCU | `kernel/sync/rcu.h` | Read-copy-update with grace periods | **Reference only** | Interesting for lock-free read-mostly engine data (e.g. live config), but RCU's grace-period model is tied to the scheduler. If needed, prefer `std::atomic<shared_ptr>` or a hazard-pointer lib. |
| Lockdep (lock-order auditor) | `kernel/sync/lockdep.h` | Runtime deadlock-cycle detector | **Reference only** | Genuinely useful concept for debug builds. But TSan (already a CI job) catches lock-order inversions on the engine's platforms without bespoke instrumentation. |
| WorkPool | `kernel/sched/workpool.h` | N-worker bounded-FIFO thread pool | **Skip** | SparkEngine already has a `JobSystem` (see `Job-System` wiki page) that is integrated with the frame loop and Wine fallbacks. A second pool would compete with it. |

## Asset / Data Codecs

| Component | DuetOS path | What it is | Verdict | Why / why-not |
|-----------|-------------|------------|---------|---------------|
| DEFLATE / GZIP / ZIP | `kernel/util/{deflate,gzip,zip}.*` | Decompression | **Skip** | SparkEngine already vendors `miniz` and `zstd` (see [ThirdParty-Dependencies-Audit](../advanced/ThirdParty-Dependencies-Audit.md)). |
| PNG / BMP / TGA / JPEG decode | `kernel/util/{png,bmp,tga,jpeg}.*` | Read-only image loaders | **Skip** | SparkEngine uses `stb_image` (+ `tinyexr`, Basis transcoder). Duplicative. |
| Base64 / CRC32 / Adler32 | `kernel/util/{base64,crc32,adler32}.*` | Encoding/checksums | **Skip** | Trivially available; `miniz`/`zstd` already provide CRC. |
| Crypto (AES/SHA/RSA/X.509) | `kernel/crypto/*` | Full crypto suite | **Skip** | An engine doesn't need a kernel-grade crypto stack; if save-file signing is ever wanted, use a maintained lib, not kernel code. |

## Diagnostics

| Component | DuetOS path | What it is | Verdict | Why / why-not |
|-----------|-------------|------------|---------|---------------|
| Kernel logging | `kernel/log/` | Severity/component-tagged async ring-buffer logging | **Skip** | SparkEngine has `Utils/Logger` (sinks, file rotation) + `SimpleConsole` IPC. No gap. |
| Cleanroom trace ring buffer | `kernel/diag/` | Low-overhead `(timestamp,event)` capture | **Reference only** | The pattern is a nice complement to the existing `Profiler` for frame-spike capture, but the engine `Profiler` + Tracy-style tooling already cover this space. |

## What SparkEngine *Should* Take From DuetOS

The durable wins are **process and documentation**, not code:

1. **The wiki discipline** — DuetOS's `_Template.md` audience/maturity header, `_Sidebar.md` master TOC, and `docs/sync-wiki.sh` AUTO-block synchronization are the model this very migration adopts. SparkEngine already had a comparable wiki; this effort brings the `.claude/knowledge` content fully into it.
2. **`// GAP:` / `// STUB:` marker conventions** — DuetOS distinguishes "not implemented yet" from "deliberately partial." SparkEngine could adopt the same two-marker convention to make [Stub-and-Abandoned-Features](../advanced/Stub-and-Abandoned-Features.md) auto-discoverable.
3. **`defer`/`ScopeGuard` ergonomic** — the one code idiom worth (re)writing natively if a contributor wants it.

## Related Pages

- [Third-Party Library Evaluation](Third-Party-Library-Evaluation.md)
- [Engine & Renderer Landscape](Engine-and-Renderer-Landscape.md) *(deep external research)*
- [Advanced Techniques Catalog](Advanced-Techniques-Catalog.md)
- [ThirdParty Dependencies Audit](../advanced/ThirdParty-Dependencies-Audit.md)

## Source & Freshness

- **Origin:** Evaluation of `C:\Users\natew\source\repos\DuetOS` (sibling OS project) against the SparkEngine codebase.
- **Compiled:** 2026-06-08.
- **Note:** Verdicts reflect SparkEngine's current mature state (full `std::`, EnTT, Jolt, JobSystem, Logger/Profiler, `std::expected`). Revisit only if a specific, profiled gap appears that a DuetOS component uniquely fills.
