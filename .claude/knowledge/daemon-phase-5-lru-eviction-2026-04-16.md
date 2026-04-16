# SparkDaemon Phase 5 — LRU Cache Eviction

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-2a-shader-service-2026-04-16.md`, `daemon-phase-3a-asset-service-2026-04-16.md`

## TL;DR

Both cache services (Shader, Asset) now bound total bytes via a
classic list+map LRU. Opt-in via `--shader-cache-max-mb` /
`--asset-cache-max-mb`; zero means unbounded (default, preserves
prior behaviour). Disk files for evicted entries are removed too.
10 new tests, full suite 5561/5561.

## Storage refactor

Replaced the plain `unordered_map<Key, vector<uint8_t>>` with the
canonical LRU pair:

```cpp
std::list<Entry>              m_lruList;   // front = most-recent
std::unordered_map<Key, Iter> m_index;     // O(1) key lookup
```

Access pattern:

| Operation | Steps |
|-----------|-------|
| Get hit | `m_lruList.splice(begin(), m_lruList, it)` to promote |
| Put existing | Replace blob, splice to front |
| Put new | `push_front`, insert index entry |
| Evict | `pop_back`, erase index, delete disk file if disk-backed |

O(1) per operation. Same thread-safety model as before — mutex
covers the whole container pair.

## Eviction trigger

`EvictUntilUnderBudget` runs after every mutation that grew the
cache (`InsertOrReplace`, `SetMaxBytes`). While `m_totalBytes >
m_maxBytes && !m_lruList.empty()`, we pop the back entry, decrement
totals, erase from the index, and (if disk-backed) remove the
corresponding `.blob` / `.asset` file.

`m_maxBytes == 0` means unbounded — `EvictUntilUnderBudget` short-
circuits immediately. That's the default, which preserves the
prior Phase 2/3 behaviour bit-for-bit.

## Self-eviction corner case

A single `Put` can submit a blob larger than the budget. The LRU
correctly evicts its way down to empty, including the entry that
just arrived. Test: `ShaderLRU_OverBudgetSinglePutEvictsItself`.

This means the cache has a **well-defined invariant**: after any
`Put` returns, total bytes ≤ max bytes. Callers that hit this case
get a `PutCacheEntryResponse` with no error (the put formally
succeeded) but a subsequent `Get` will miss — the client's next
action would be to recompute and `Put` again, which is the same
code path it would take if the daemon had rejected the put outright.
Simpler semantics, no new error code.

## Disk-eviction coupling

When `m_diskBacked`, every eviction also removes the backing file
via `DeleteBlobFile(key)`. The write path does the inverse: after
insert + evict, we check whether the just-inserted entry survived
— if yes, write its file; if no (it evicted itself), remove any
stale file under the same key.

Test `ShaderLRU_DiskFilesRemovedOnEviction` / the asset equivalent
verifies on-disk `.blob` / `.asset` counts stay in lockstep with
in-memory entry counts.

## SetMaxBytes after Initialize

`Initialize(cacheDir)` loads every `.blob` / `.asset` file it finds,
ignoring any future budget. Calling `SetMaxBytes` afterwards runs
`EvictUntilUnderBudget` immediately, which trims down to fit — both
in memory AND on disk. Test:
`AssetLRU_ReloadAfterRestartRespectsLimit`.

Rationale: Initialize is the cold-start path; we always want to see
whatever's on disk before applying policy. Enforcing the budget
only after load lets operators change the limit across restarts
without manually pruning the cache directory.

## Stats counter

New `m_evictionCount` atomic, bumped on each eviction. Not yet
plumbed through the `GetCacheStats` RPC response struct (needs a
wire-format bump — follow-up). Available internally for future
diagnostics panels and structured logging.

## CLI

```
SparkDaemon [--socket <path>]
            [--cache-dir <path>] [--shader-cache-max-mb <N>]
            [--asset-cache-dir <path>] [--asset-cache-max-mb <N>]
```

`<N>` is MB. Zero / omitted = unbounded. Typical dev setup:

```bash
SparkDaemon \
    --cache-dir ~/.cache/spark/shaders  --shader-cache-max-mb 512 \
    --asset-cache-dir ~/.cache/spark/assets --asset-cache-max-mb 4096
```

## Test coverage

### Shader (5 tests)

| Test | What it verifies |
|------|-----------|
| `ShaderLRU_PutOverflowEvictsOldest` | 4th entry evicts the 1st (oldest) entry |
| `ShaderLRU_GetPromotesRecency` | Get on oldest makes it newest; next eviction targets the formerly-2nd entry |
| `ShaderLRU_OverBudgetSinglePutEvictsItself` | Single blob > budget self-evicts; cache ends empty |
| `ShaderLRU_SetMaxBytesTrimsExistingCache` | Shrinking budget on a live cache drops entries immediately |
| `ShaderLRU_DiskFilesRemovedOnEviction` | `.blob` files removed in lockstep with in-memory eviction |

### Asset (5 tests)

| Test | What it verifies |
|------|-----------|
| `AssetLRU_PutOverflowEvictsOldest` | Same as shader, with path+platform keys |
| `AssetLRU_GetPromotesRecency` | Same as shader |
| `AssetLRU_InvalidateCoexistsWithEviction` | `InvalidateAsset` after eviction pressure still removes all variants for a path correctly |
| `AssetLRU_DiskFilesRemovedOnEviction` | `.asset` files removed in lockstep |
| `AssetLRU_ReloadAfterRestartRespectsLimit` | Restart → Initialize loads everything; SetMaxBytes trims to fit both memory and disk |

Tests drive the service's `HandleMessage` directly (no socket/server
round-trip) because LRU is a pure in-process data-structure
invariant — the wire layer would just add 500 ms per test for no
extra coverage.

## Not yet

- **`GetCacheStats` wire format doesn't include eviction count.**
  Adding `evictionCount` to `ShaderCacheStats` / `AssetCacheStats`
  requires a version bump on both payload codecs. Skipped this slice
  because it touches Phase 2 / 3 contracts — separate commit.
- **No access-time promotion on `PutAsset` overwrite.** Same key
  put twice promotes correctly (tested), but the existing semantic
  of "overwrite = freshness reset" is inherent to LRU — intentional.
- **Size budget excludes key storage overhead.** Only blob bytes
  count toward `m_maxBytes`; the `Key` structs and `unordered_map`
  nodes add a few dozen bytes per entry that aren't tracked. Matters
  only for pathological caches with tens of thousands of tiny
  entries; the ~1:100 ratio of overhead to blob size is negligible
  for typical shader / asset sizes.
