# SparkDaemon Phase 3a — Asset Cache Service

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-2a-shader-service-2026-04-16.md`, `daemon-phase-2b-shader-persistence-2026-04-16.md`

## TL;DR

Second production service on the daemon. Asset blob cache keyed by
`(logical path, platform)` with an extra `InvalidateAsset(path)` RPC
that drops every platform variant for a given source path in one
round-trip. Disk persistence available via `--asset-cache-dir`. 10
new tests, full suite 5532/5533.

## API surface

`AssetServiceClient` (engine side, wraps a `DaemonClient&`):

| Method | Wire message | Notes |
|---|---|---|
| `GetAsset(path, platform)` | `0x0001 → 0x0002` | Miss returns `found=false, blob={}` |
| `PutAsset(path, platform, blob)` | `0x0003 → 0x0004` | Overwrites existing entry |
| `InvalidateAsset(path)` | `0x0005 → 0x0006` | Drops every platform variant; returns removed count |
| `GetCacheStats()` | `0x0007 → 0x0008` | Entry count, total bytes, hits, misses |

`path` is whatever string the engine uses to identify a source asset
(typically a VFS path like `"Textures/brick.png"`). `platform` is an
opaque `uint8_t` — daemon doesn't know what it means.

## Disk layout

```
<assetCacheDir>/<pathHash16>_<platform3>.asset
file contents: [u32 pathLen LE][pathBytes][blobBytes]
```

- `<pathHash16>` — 16 hex digits of FNV-1a-64 of the path bytes
- `<platform3>` — 3-digit zero-padded platform ID

### Why path-hash + path-in-file, not the path alone

Filenames on Linux are capped at 255 bytes (NAME_MAX). A logical
asset path can easily exceed that — `TestLongPathSurvivesDiskRoundTrip`
uses a 500+ character path. Using the FNV-1a hash for the filename
keeps every on-disk name under 25 bytes, and storing the original
path as a header inside the file lets us rebuild the `(path, platform)`
in-memory key on startup without a separate index.

FNV-1a collisions are possible in principle but astronomically
unlikely in practice. Correctness is preserved because the in-memory
map is keyed by the full path string, so two colliding paths have
distinct entries in memory — only the disk file is shared, and
whichever `PutAsset` ran last wins on disk. For a cache, that's
an acceptable trade.

## Divergences from Phase 2 (shader)

| Aspect | ShaderService (Phase 2) | AssetService (Phase 3a) |
|---|---|---|
| Primary key | `(uint64_t hash, uint8_t target, uint8_t stage)` | `(std::string path, uint8_t platform)` |
| File name | `<hash>_<target>_<stage>.blob` | `<pathHash>_<platform>.asset` |
| File format | Raw bytes only | `[pathLen][pathBytes][blob]` — path embedded |
| Drop operation | `Clear` (whole cache) | `Invalidate(path)` (per-source) + no Clear RPC yet |
| Hit/miss counters | Relaxed atomics | Relaxed atomics (same pattern) |
| Write pattern | `.tmp` stage + atomic rename | `.tmp` stage + atomic rename |

The `Invalidate(path)` semantic is the interesting new piece — it
matches the architecture plan's motivation: when a source `.png`
changes on disk, one RPC invalidates the Windows / Linux / Android
variants in a single round-trip. In-process code used to have to
re-key across platforms manually.

## CLI

```
SparkDaemon [--socket <path>] [--cache-dir <path>] [--asset-cache-dir <path>]
```

Each cache dir is independent and opt-in. Typical dev-machine usage:

```
SparkDaemon \
  --socket /tmp/spark-daemon.sock \
  --cache-dir ~/.cache/spark/shaders \
  --asset-cache-dir ~/.cache/spark/assets
```

## Test coverage

| Test | What it verifies |
|---|---|
| `AssetService_PutThenGetRoundTrip` | In-memory put then get yields identical bytes |
| `AssetService_MissReturnsFoundFalse` | Unknown key → `found=false`, empty blob |
| `AssetService_DifferentPlatformsAreDistinct` | Same path, 3 platforms = 3 separate entries |
| `AssetService_InvalidateDropsAllPlatformVariants` | Invalidating `sound.wav` removes all 3 platform variants; unrelated `other.wav` survives |
| `AssetService_InvalidateUnknownPathReturnsZero` | Invalidate on missing path returns `removedCount=0` |
| `AssetService_StatsHitsAndMisses` | Counters track correctly across hits and misses |
| `AssetService_PutWritesFileToDisk` | `PutAsset` creates exactly one `.asset` file |
| `AssetService_ReloadsFromDiskOnRestart` | 3 entries survive a full server stop + restart |
| `AssetService_InvalidateRemovesFilesFromDisk` | Invalidate deletes the files from the filesystem |
| `AssetService_LongPathSurvivesDiskRoundTrip` | 500+ char path round-trips through disk correctly |

## Pattern validation

Phase 3a validates that the wrapper pattern from Phase 2 scales:
- Write the `<Service>Protocol.h` with message enum + codec
- Write the `<Service>ServiceClient.{h,cpp}` with typed RPC methods
- Write the daemon-side `<Service>Service.{h,cpp}`
- Register in `main.cpp`; add a `--<service>-cache-dir` flag for persistence
- Loopback tests using a local `DaemonServer` instance

A third service (Collab, Build) can follow the same template verbatim.

## Not in Phase 3a

- **LRU eviction** — unbounded growth on disk, same as shader cache
- **Cook pipeline** — no actual asset compression / transcoding /
  mip-generation; daemon just stores opaque blobs. The engine still
  runs its own `AssetPipeline` in-process and would `PutAsset` the
  result.
- **File watching** — source `.png` / `.obj` / `.wav` changes don't
  auto-invalidate. Client must call `InvalidateAsset` explicitly.
- **Engine integration** — `AssetCache` in the engine does not yet
  consult the daemon; this wrapper is available but unwired.
