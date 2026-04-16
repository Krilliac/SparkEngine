# SparkDaemon Phase 5 Follow-up — evictionCount on the Wire

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-5-lru-eviction-2026-04-16.md`

## TL;DR

`ShaderCacheStats` and `AssetCacheStats` now carry `evictionCount` over
the wire. Decoders tolerate legacy 32-byte payloads (daemon without
the field) and leave `evictionCount` at zero — no protocol version
bump needed, older clients and daemons inter-operate. 5 new tests,
full suite 5570/5570.

## Motivation

Phase 5 added an `m_evictionCount` atomic to both cache services but
did not surface it in `GetCacheStatsResponse`; the original phase
knowledge file listed this as a follow-up. Eviction pressure is a
direct signal of whether the operator-picked cache budget is too small
— without it on the wire, diagnostics panels and CI cache-efficiency
reports have no way to see it.

## Wire change

Both payload structs gained one trailing `uint64_t`:

```cpp
struct ShaderCacheStats  // same for AssetCacheStats
{
    uint64_t entryCount     = 0;
    uint64_t totalBytes     = 0;
    uint64_t hitCount       = 0;
    uint64_t missCount      = 0;
    uint64_t evictionCount  = 0;   // NEW
};
```

Encoders always emit 40 bytes. Decoders read the first four fields
with the usual `r.Read<uint64_t>()` pattern, check `HasError()`, then:

```cpp
if (r.Remaining() >= sizeof(uint64_t))
    out.evictionCount = r.Read<uint64_t>();
```

`BinaryReader::Remaining()` short-circuits to zero when the reader is
in an error state, so the conditional is safe in both directions.
Result: newer clients against older daemons receive `evictionCount=0`
silently; older clients against newer daemons ignore the trailing
bytes (`BinaryReader` doesn't flag trailing data as an error).

## Why no protocol-version bump

The wire rule in `DaemonProtocol.h` calls for a semver bump only when
the **frame header** layout changes. Appending trailing fields to a
payload is an additive change covered by the existing
`HasError()`/`Remaining()` decoder idiom. All other payload codecs in
the daemon follow the same convention.

## `ClearCache` semantics

`ShaderService::HandleClearCache` already resets `m_evictionCount` to
zero alongside `m_hitCount` / `m_missCount` (added in Phase 5). The
wire exposure of the field made it worth pinning down via a dedicated
test (`ShaderLRU_ClearCacheResetsEvictionCount`) so the behavior is
captured before any consumer depends on the counter never resetting.

## New tests

| Test | What it verifies |
|------|------------------|
| `ShaderLRU_StatsReportEvictionCount` | `GetCacheStats` reports `evictionCount == evictions observed by test` |
| `ShaderLRU_ClearCacheResetsEvictionCount` | `ClearCacheRequest` zeros the counter |
| `AssetLRU_StatsReportEvictionCount` | Asset variant of the shader test |
| `ShaderCacheStats_DecoderToleratesLegacyPayload` | Decoding a 32-byte (pre-follow-up) payload leaves `evictionCount=0` and returns `true` |
| `AssetCacheStats_DecoderToleratesLegacyPayload` | Asset variant of the legacy-payload test |

Tests exercise the full `HandleMessage` + decoder path (same style as
the existing LRU tests). No socket round-trip — the wire framing is
not affected by this change, so round-trip adds no coverage.

## Not yet / deferred

- Counter is still an atomic `uint64_t`; no stream of eviction
  **events** is exposed (would be useful for a live diagnostics HUD
  but needs a push/subscribe channel that doesn't exist yet in the
  daemon).
- No editor UI panel for daemon stats — adding one lives with the
  broader "daemon observability" slice, not this follow-up.
