# SparkDaemon Phase 2b — Shader Cache Disk Persistence

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-phase-2a-shader-service-2026-04-16.md`

## TL;DR

Phase 2b adds optional on-disk persistence to the shader cache service.
Warm cache now survives daemon restarts. Opt-in via `--cache-dir <path>`
on the SparkDaemon CLI; tests with no cache directory still get
in-memory-only behaviour (Phase 2a semantics preserved). 5 new tests,
all 11 Shader service tests pass, full suite 5522/5523.

## Disk layout

One file per cache entry under the supplied directory:

```
<cacheDir>/<hash16>_<target3>_<stage3>.blob
```

- `<hash16>` — 16 lowercase hex digits (`%016llx`) of the source hash
- `<target3>` — 3-digit zero-padded target ID (`%03u`, 000–255)
- `<stage3>` — 3-digit zero-padded stage ID (`%03u`, 000–255)

Raw blob bytes only — no header or schema envelope. The daemon is a
dumb byte store; any format versioning lives above it.

## Startup scan

`ShaderService::Initialize(cacheDir)`:
1. `create_directories(cacheDir)` — idempotent, creates chain if missing.
2. Iterate files with `.blob` extension.
3. Parse the stem with `ParseBlobFilename()`; skip anything that doesn't
   match the strict 24-char `<hex16>_<dec3>_<dec3>` grammar.
4. Read the file into a `vector<uint8_t>` and insert into `m_entries`.
5. Return count loaded (or `nullopt` if the path is not a directory).

Invalid filenames, wrong extensions, and read failures are silently
skipped — the cache is a rebuild-safe speed cache, not authoritative
data. One corrupt file does not abort startup.

## Write path

`HandlePutCacheEntry` now splits into:
1. Critical section updates the in-memory map and bumps `m_totalBytes`.
2. A **copy** of the just-stored blob is kept under the lock (see below).
3. Outside the lock, `WriteBlobFile(key, blob)` stages to a `.tmp` file
   and `rename()`s it over the final path.

The atomic rename means a crash mid-write leaves either the old blob
or no blob — never a half-written one that the next startup would load
as valid.

### Why copy the blob for disk write

We hold the service mutex just long enough to update the map. Doing
the `ofstream.write` call under the lock would serialise every
`PutCacheEntry` request against every concurrent `GetCacheEntry`.
Copying the blob (typical shader: kilobytes to low MB) is cheap
compared to the disk write and lets readers proceed in parallel.

The trade-off: if the daemon crashes between the in-memory update and
the disk flush, the next startup will be missing the entry. Clients
can't observe this as a correctness bug — they just recompute and call
`PutCacheEntry` again. Good enough for a speed cache.

## Clear path

`HandleClearCache` now also calls `DeleteAllBlobFiles()` which iterates
the directory and removes every `.blob` file. Failures on individual
files are ignored — the in-memory cache is already empty, so the
worst case is some leftover files that will be overwritten or
re-scanned (as already-known entries) on next startup.

## CLI

```
SparkDaemon [--socket <path>] [--cache-dir <path>]
```

`--cache-dir` is purely opt-in. Without it, the ShaderService runs in
its original Phase 2a in-memory-only mode. This matches the broader
principle: daemon features must never be required — the engine falls
back to in-process behaviour when no daemon is running, and the daemon
falls back to ephemeral behaviour when no cache directory is set.

## Test coverage added

| Test | What it verifies |
|---|---|
| `ShaderService_PutWritesFileToDisk` | `PutCacheEntry` creates a `.blob` file with the correct bytes |
| `ShaderService_ReloadsFromDiskOnRestart` | Three entries written → server torn down → new server loads the same three entries with identical blobs |
| `ShaderService_ClearRemovesBlobFiles` | `ClearCache` removes the files from the filesystem, not just the in-memory map |
| `ShaderService_OverwriteReplacesFileContents` | Same key put twice → restart → only the second value survives |
| `ShaderService_MalformedFilenameIsIgnoredOnLoad` | Junk files in the cache dir don't break startup; only valid-format `.blob` files are loaded |

Four of the five spin up a real `DaemonServer`, Put some entries, tear it
down, start a new `DaemonServer` on the same cache dir, and verify the
entries round-trip — this is the end-to-end persistence story.

## Follow-up candidates

- **LRU eviction** — the on-disk cache currently grows without bound.
- **Corruption detection** — add a CRC32 footer in the blob file and
  drop entries whose checksum doesn't match. Right now we trust the
  filesystem.
- **Parallel preload** — startup scan is serial. For projects with tens
  of thousands of cached shader blobs, a thread pool on the load could
  shave seconds off `Initialize()`.
- **File watchers** — the original Phase 2 plan includes file watchers
  on `.hlsl` sources to invalidate cache entries when source changes.
  Needs a cross-platform inotify/FSEvents/kqueue wrapper we don't have yet.
