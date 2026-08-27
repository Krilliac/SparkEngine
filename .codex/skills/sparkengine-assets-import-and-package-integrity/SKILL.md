---
name: sparkengine-assets-import-and-package-integrity
description: >-
  SparkEngine asset ingestion & package-integrity runbook: the AssetPipeline load path and
  Windows/Linux loader split, hardened importer/decoder limits (FBX/EXR/Basis), SparkPak
  (.spk) archive format and corruption defenses, VFS path-containment rules, the two
  divergent GamePackagers, the daemon asset cache (DDC analog), and what failure evidence
  to collect. TRIGGER when: "mesh/texture won't load", "how does AssetPipeline::LoadAsset
  work", "malformed/corrupt asset crashes the engine", "fuzz an importer", "mount or build
  a .spk / pak archive", "path traversal in asset paths", "validate assets / asset
  integrity", "package smoke test failed", "cook assets", "asset cache / DDC /
  daemon.invalidate", "why doesn't the engine load from the pak", "is asset packaging
  release-ready". DO NOT TRIGGER when: asking about GPU upload / render graph / shader
  authoring (use sparkengine-rendering-rhi-rendergraph-and-shaders), CMake/CPack
  configure or dependency breakage (use sparkengine-build-ci-and-dependencies), save-file or
  scene-format version migrations (use sparkengine-persistence-save-and-migrations), or test
  registration mechanics (use sparkengine-validation-and-qa).
---

# SparkEngine — Assets, Import, and Package Integrity

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

Runbook for the *integrity* layer of SparkEngine content: what defends the engine against
malformed asset bytes, how the SparkPak archive format works and what its reader rejects,
where path containment is enforced, which caches exist, how packaged builds are validated,
and — critically — which of these are implemented, tested, CI-enforced, or merely candidate.
This skill also owns the asset *load* mechanics (`AssetPipeline::LoadAsset`, the
`AssetPipelineWindows.cpp` / `AssetPipelineLinux.cpp` loader split, adding a source
format) — there is no separate asset-pipeline skill.
All paths repo-relative. Verified 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`).

Terms defined once:

- **SparkPak / `.spk`** — the engine's MPQ-inspired binary archive format
  (`SparkEngine/Source/Core/SparkPak.h`): 32-byte header, raw data blobs, deflate/zstd-compressed
  table of contents (TOC) at end of file, O(1) lookup by 64-bit FNV-1a hash of the virtual path.
- **VFS** — `Spark::VirtualFileSystem` (`SparkEngine/Source/Engine/Modding/VirtualFileSystem.h`),
  a mount-priority virtual filesystem layering loose directories (`LocalFileProvider`) and
  archives (`ArchiveResourceProvider`).
- **Daemon asset cache** — SparkEngine's DDC (derived-data-cache) analog: an out-of-process
  blob cache inside SparkDaemon (`SparkDaemon/src/AssetService.h`) keyed by
  `(logical path, platform)`, reached via `Spark::Daemon::AssetServiceClient`.
- **PackageSmoke** — a standalone CMake consumer project (`Tests/PackageSmoke/`) that builds
  against the *installed* SDK and executes real Spark symbols, proving package integrity.
- **Readiness contract** — `docs/site/readiness.json` + `docs/readiness/work-items/*.json`;
  the authoritative ledger for any public claim (see
  `sparkengine-change-control-and-release-readiness`).

## When NOT to use this skill

| You are doing | Use instead |
|---|---|
| RHI backends, render graph, shader compilation | `sparkengine-rendering-rhi-rendergraph-and-shaders` |
| CMake presets, CPack config, ThirdParty manifest, CI build breaks | `sparkengine-build-ci-and-dependencies` |
| Save/scene serialization format versions and migrations | `sparkengine-persistence-save-and-migrations` |
| Registering/filtering tests, sanitizer lanes, coverage gates | `sparkengine-validation-and-qa` |
| Promoting a claim ("packaging is done") | `sparkengine-change-control-and-release-readiness` |

## Status ledger — read this before believing anything

| Capability | Status (2026-08-23) | Evidence anchor |
|---|---|---|
| FBX binary parser hardening (bounds-checked reader, decode limits) | **Implemented + tested** — 17 malformed-input tests exercise the real importer | `SparkEngine/Source/Graphics/FBXImporter.h` (`FBXBinaryReader::SetLimit`, `kMaxDecodedBytes` = 128 MB, `kMaxProperties` = 250,000); `Tests/TestFBXImporter.cpp` (registered `Tests/CMakeLists.txt:351`) |
| EXR loader input limits | **Implemented** — 16 MB input cap, 16,384 px dimension cap, 128 MB working-set cap | `SparkEngine/Source/Graphics/EXRLoader.h:97-126` |
| SparkPak reader corruption defenses | **Implemented + production-linked tests** — bounded TOC/allocations, shared-`FILE*` access serialized by a mutex; `Tests/TestSparkPak.cpp` now links the real `Spark::SparkPakReader`/`SparkPakWriter` in addition to its older format-mirror section (see note below) | `SparkEngine/Source/Core/SparkPak.cpp` (`kMaxTocBytes`, `m_fileMutex`); `Tests/TestSparkPak.cpp` (registered in `Tests/CMakeLists.txt`) |
| SparkPak writer | **Implemented + production-linked tests**; packaging refuses symlinks and verifies entries cannot escape the source tree (`SparkPakWriter.cpp` `AddDirectory`). Still **no production cook tool invokes it** — pak cooking end-to-end remains unexercised | `SparkEngine/Source/Core/SparkPakWriter.cpp`; `Tests/TestSparkPak.cpp` |
| VFS path containment (`..` and absolute-path rejection) | **Implemented** (loose-file provider) | `SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp:33-48` |
| `.spk` auto-mount at startup | **Implemented + wired** (`<cwd>/Data/*.spk`) | `SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp:194,479` |
| Engine asset loading *through* the VFS/pak | **Not wired** — `AssetPipeline` reads the OS filesystem directly; only console commands consume the VFS | grep below returns no `VirtualFileSystem` use in `Graphics/Asset*`/`ModelLoading*` |
| AssetValidator (`assetvalidate.*` console commands) | **Implemented + wired**, but rules are shallow existence/size heuristics — not content decoding | `SparkEngine/Source/Core/AssetValidator.cpp`; registered in all four init paths (e.g. `SparkEngineWindowsInit.cpp:274`) |
| In-process `AssetCache` (LRU) | **Implemented + tested + on the load path** | `SparkEngine/Source/Graphics/AssetPipeline.h:309`; hits at `AssetPipelineWindows.cpp:109`, `AssetPipelineLinux.cpp:126` |
| Daemon asset cache (DDC analog) | **Implemented (Phase 3a: cache ops only)**; console-reachable; **not consulted by any load path** | `SparkDaemon/src/AssetService.h`; `SparkEngine/Source/Utils/AssetServiceClient.h`; no `PutAsset` caller outside `Utils/DaemonLifecycle.cpp` |
| PackageSmoke installed-SDK validation | **Implemented + CI-enforced in the release workflow** (not per-PR) | `Tests/PackageSmoke/main.cpp`; `.github/workflows/release.yml:108,168` |
| GamePackager | **Two divergent implementations** — `Spark::Build::GamePackager` (wired + tested), `Spark::GamePackager` (Core, zero external callers) | see "Package assembly" below |
| `spark package --compress` (SparkPak cooking via CLI) | **Stub** — records a `"compressed"` flag in `manifest.json`, never creates `.spk` | `Tools/spark-cli/spark_cli.py:196-315` |
| Asset/package release readiness | **Blocked** — gate `G04` blocked; work items `RDY-020`, `ASSET-220` open P0 | `docs/site/readiness.json` (G04); `docs/readiness/work-items/00-truth-ci-release.json` (RDY-020), `20-platform-runtime-editor.json` (ASSET-220) |

> **The mirror-test note (updated 2026-08-23).** `Tests/TestSparkPak.cpp` still contains a
> format-mirror section (an anonymous-namespace "standalone reimplementation" of the pak
> header/FNV-1a for format-level checks), but it now **also links and exercises the real
> `Spark::SparkPakReader` and `Spark::SparkPakWriter`** — production round-trip and
> rejection paths are covered. Keep the distinction in mind: only the production-linked
> tests can promote readiness (mirror-only tests cannot, per
> `docs/site/readiness.json`). When touching `SparkPak.cpp`/`SparkPakWriter.cpp`, extend
> the production-linked tests, and keep the mirrored format structs in sync with any
> on-disk layout change.

## SparkPak format contract (v1)

Verified against `SparkEngine/Source/Core/SparkPak.h`:

| Field | Value |
|---|---|
| Magic | `0x314B5053` ("SPK1" little-endian), version `1` |
| Header | 32 bytes at offset 0, `#pragma pack(1)`, `static_assert(sizeof(PakHeader) == 32)` |
| Layout | `[Header][file data blobs...][compressed TOC at end]` (ZIP-style central directory) |
| TOC entry (on disk) | `pathHash(8) + dataOffset(8) + compressedSize(4) + originalSize(4) + compression(1) + pathLen(2) + path` — minimum 27 bytes |
| Path hash | 64-bit FNV-1a (`PakFNV1a`), documented to match `AssetHandle::FromPath` |
| Compression | `Stored=0`, `Deflate=1` (miniz), `Zstd=2` — both third-party libs are vendored (`ThirdParty/Utils/miniz`, `ThirdParty/Utils/zstd`) and gated by `SPARK_MINIZ_AVAILABLE` / `SPARK_ZSTD_AVAILABLE` (`CMakeLists.txt:1238,1252`) |
| Writer heuristic | Compression skipped when compressed size ≥ 95% of original (`SparkPakWriter.h`) |

Reader corruption defenses (all in `SparkPak.cpp::ReadTOC`/`ReadFile` — header fields are
treated as untrusted input):

- TOC size/rawSize capped at 256 MB; `fileCount` capped at 10,000,000.
- `tocOffset`/`tocOffset+tocSize` checked against physical file size before any allocation.
- Hash-map reserve bounded by `tocRaw.size() / 27` so a lying `fileCount` cannot balloon memory.
- Per-entry read caps: `compressedSize`/`originalSize` ≤ 2 GB.
- Without miniz, a compressed TOC (tocSize ≠ tocRawSize) is rejected outright.
- Any violation → `Open()` returns `false` and logs `SparkPakReader: invalid header/TOC in '<path>'`.
- Per-entry reads take an internal mutex around seek+read (`SparkPak.cpp`, "A FILE* has
  one shared cursor") so concurrent readers on the shared `FILE*` cannot interleave.

Writer-side defenses (`SparkPakWriter.cpp::AddDirectory`): file symlinks are not
followed while packaging, and entries are verified against platform-specific
indirections so packaged content cannot escape the source tree.

Known sharp edges (do not "fix" silently — record a work item):

- **FNV-1a collision policy**: reader keys entries by hash only; colliding paths silently
  shadow each other (`m_entries.emplace`). Daemon cache has the same hash but keys the
  in-memory map by full path string; disk files are last-write-wins.
- **`ListFiles` prefix match is raw**: `virtualPath.find(directory) != 0` — `"tex"` matches
  `"textures/brick.png"`. There is no path normalization inside the archive namespace.
- **Version bump rule**: any on-disk layout change must increment `kSparkPakVersion`; the
  reader rejects version mismatches (no migration path exists — that would be
  `sparkengine-persistence-save-and-migrations` territory to design, not here to improvise).

## Path containment

Enforced in exactly one place for loose files: `LocalFileProvider::ResolvePath`
(`VirtualFileSystem.cpp:33-48`):

- Any `..` anywhere in the virtual path → rejected, returns empty, logs
  `VFS: rejected path traversal attempt '<path>'` (deliberately fails instead of clamping to
  the sandbox root, which previously masked attacker intent).
- Leading `/` or `\` (absolute path) → rejected, logs `VFS: rejected absolute path`.

Containment facts to keep straight:

- `ArchiveResourceProvider` needs no traversal check — pak lookups are hash-based against a
  fixed TOC; there is no filesystem dereference per entry.
- **The check only protects VFS consumers.** `AssetPipeline`, scene serializers, and script
  loaders that take raw paths bypass it entirely. Readiness item `RDY-020` ("Tampered or
  traversal paths fail before package assembly") is `open` — repo-wide containment is a
  **candidate**, not a property.
- VFS mount priorities: `ENGINE_PRIORITY=0`, `GAME_PRIORITY=100`, `DLC_PRIORITY=200`,
  `MOD_PRIORITY=300` (`VirtualFileSystem.h:47-50`). Highest wins; if a higher-priority mount's
  read fails, the VFS logs a WARN and falls back to the next mount.

## Startup mount behavior

`MountSparkPakArchives()` (`GameplayLifecycleShared.cpp:194`, called at line 479 right after
`VirtualFileSystem::Initialize()`):

1. Scans `<current working directory>/Data/` for `*.spk` (note: CWD, not executable dir).
2. Sorts lexicographically; mounts each at `ENGINE_PRIORITY + index` (so `a.spk` < `b.spk`
   in priority — later alphabetical names win ties).
3. Valid archives log `[SparkPak] Mounted: <name> (priority N)` to the console; invalid ones
   log `[SparkPak] Failed to open: <name>` and are skipped (engine continues).

No `.spk` files exist in the repository, and nothing production-side calls `SparkPakWriter`,
so in practice this scan finds nothing unless you hand-build an archive. That makes
"pak-based content delivery" **implemented but unexercised end-to-end** — do not claim it works
in a shipped layout without producing one and checking `pak_status`.

## Caches

Two unrelated caches — don't conflate them:

1. **In-process `AssetCache`** (`AssetPipeline.h:309`) — LRU over loaded `Asset` objects,
   consulted at the top of every `LoadAsset` (`AssetPipelineWindows.cpp:109`,
   `AssetPipelineLinux.cpp:126,299`). Memory-budgeted, hit/miss counters. Real tests:
   `Tests/TestAssetPipelineCache.cpp` (eviction, MRU preservation, memory tracking).
   Invalidation on source change is timestamp polling (`CheckForChangedAssets`) — real on
   the Linux path, still a stub on Windows (verify in `AssetPipelineWindows.cpp` —
   loader-split mechanics are owned by this skill).
2. **Daemon asset cache** (DDC analog) — `SparkDaemon/src/AssetService.{h,cpp}`:
   `(path, platform)`-keyed blob store, optional on-disk backing
   (`<cacheDir>/<pathHash16>_<platform3>.asset`, self-describing: `[u32 pathLen][path][blob]`,
   rebuilt on restart without an index). Payload capped by `kMaxPayloadSize` (16 MiB).
   Engine facade: `Spark::Daemon::AssetServiceClient` — `GetAsset`, `PutAsset`,
   `InvalidateAsset(path)` (drops all platform variants in one RPC), `GetCacheStats`,
   `ClearCache`. **Phase 3a: cache operations only** — the header itself says cooking
   (compression, transcoding, mip generation) and file watching "land in later phases".
   Nothing in the asset load path calls `GetAsset`/`PutAsset`; the only consumers are the
   console commands in `Utils/DaemonLifecycle.cpp`. Treat "the daemon caches compiled assets
   for the engine" as **candidate**.

Console access: `daemon.stats`, `daemon.clear_cache <shader|asset|all>`,
`daemon.invalidate <path>` (names at `DaemonLifecycle.cpp:68-70`).

## Package assembly and integrity

- **Two GamePackagers exist and diverge** (readiness item `ASSET-220` calls this out):
  - `Spark::Build::GamePackager` — header-only singleton in
    `SparkEngine/Source/Engine/Build/GamePackager.h`. Lifecycle-wired: `Initialize()` at
    `GameplayLifecycleShared.cpp:523`, `Shutdown()` at `:1273`. Tested:
    `Tests/TestGamePackager.cpp`, `Tests/TestCoreAndBuildSystems.cpp` (both use the `Build::`
    one). **But `Package()` itself has no production caller** (editor/console never invoke
    it — tests only), so treat engine-side packaging as `candidate`; CPack is the shipped
    path (`sparkengine-run-package-and-release`). Collects exe + `Modules/*.dll` +
    `Assets/` + `Data/*.spk` into an output dir; the `createZip` config field exists but no
    zip code path was found in the header — verify before relying on it.
  - `Spark::GamePackager` — `SparkEngine/Source/Core/GamePackager.{h,cpp}`. **Zero callers
    outside its own files** — `open`, candidate for consolidation/deletion per `ASSET-220`;
    per repo anti-bloat rules, either wire it in or delete it — do not extend both.
- **PackageSmoke** (`Tests/PackageSmoke/main.cpp`) is the real installed-package integrity
  check: it compiles against the installed SDK (`find_package(SparkEngine)`) and executes
  reject-paths of the hardened decoders — 27 zero bytes must be refused by `EXRLoader::Load`
  and `FBXImporter::CanImportFromMemory`, plus JSON parse, ECS component, PhysicsSystem, and
  Recast linkage probes. Exit code 0 only if malformed input is rejected *and* real symbols
  ran. CI-enforced in `.github/workflows/release.yml` ("Validate external package
  consumption", Windows `:108` and Linux `:168`) — **release workflow only, not per-PR**.
- The readiness ledger's `ASSET-220` rationale still says "package smoke does not call Spark
  symbols"; that was fixed in commit `0ecdc00a` (2026-08-23). The ledger remains authoritative
  for *release claims* until promoted with same-SHA evidence — see
  `sparkengine-change-control-and-release-readiness` for the promotion procedure.

The canonical local reproduction recipe for the PackageSmoke check lives in
`sparkengine-validation-and-qa` §7 — do not duplicate it here.

## Decision rules

| Situation | Rule |
|---|---|
| Malformed asset bytes crash or hang a decoder | Fix in the decoder with an explicit bound (follow the `FBXBinaryReader`/`EXRLoader` pattern: cap input size, dimensions, decoded bytes, element counts; fail closed). Land a minimized regression test against the **real** class, not a mirror. Fuzz work is tracked as an open item (`FuzzAsset`/`FuzzArchive` selectors in `docs/readiness/work-items/10-security-network-operations.json`) — label new fuzz targets `candidate` until that lands. |
| Changing `.spk` on-disk layout | Bump `kSparkPakVersion`; update `SparkPakWriter` + `SparkPakReader` + both test sections in `Tests/TestSparkPak.cpp` (production-linked tests *and* the format-mirror structs) together; keep the 27-byte minimum-entry constant in sync everywhere. |
| "Should this content ship loose or in a pak?" | Today: loose. The engine load path does not read the VFS, so pak-only content is invisible to `AssetPipeline`. Wiring `AssetPipeline` reads through `VirtualFileSystem::ReadFile` is the prerequisite — treat as a design change, not a quick fix. |
| Adding a validation rule | Implement `IAssetValidationRule`, register in `AssetValidator::Initialize()` (`AssetValidator.cpp:194`) — one registration point per the manifest rule. Name it honestly: `ShaderCompilationValidator` only checks existence/size, and that mislabel is exactly the kind of oversell the readiness contract forbids. |
| A "pass" from `assetvalidate.*` or PackageSmoke | Ask what a failure would have looked like. `assetvalidate.dir` on a missing directory reports 1 fail (good); PackageSmoke returns 1 if any probe fails. A 0-asset validation run reporting "0 fail" proves nothing. |
| Claiming asset/package readiness | Gate `G04` is `blocked`; `RDY-020`/`ASSET-220` are `open`. Any upward claim goes through the readiness contract with same-SHA evidence — never in prose only. |

## Failure evidence — what to collect when content breaks

1. **Console state** (in-engine console or SparkConsole):
   `pak_status` (mounts + priorities), `pak_list [dir] [.ext]`, `assetvalidate.dir <path>`
   then `assetvalidate.report`, `daemon.stats` (registered in
   `EngineConsoleCommands.cpp:920-948`, `AssetValidator.cpp:397`, `DaemonLifecycle.cpp`).
2. **Log lines to grep for** (all via `SPARK_LOG_*` / console):
   `SparkPakReader: invalid header/TOC`, `SparkPakReader: failed to open`,
   `[SparkPak] Failed to open`, `VFS: rejected path traversal attempt`,
   `VFS: rejected absolute path`, `VFS: '<path>' read failed in [<mount>], falling back`.
3. **Importer failures**: `FBXImportResult.errorMessage` / `warnings` (returned, not thrown);
   `AssetStallDetector` console commands for loads that hang rather than fail.
4. **Reproduce deterministically**: prefer `ImportFromMemory`/`CanImportFromMemory` with the
   captured bytes over file-path repro — that's what the regression tests use.

## Verification commands (run from repo root)

```bash
# Build + run the registered integrity tests (SparkTests is one ctest entry)
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/windows-release -C Release --output-on-failure

# Run only this domain's tests via the SparkTests env filters
# (full selector list: sparkengine-validation-and-qa §3; preset binary: build/<preset>/bin/)
SPARK_TEST_FILE=TestSparkPak.cpp ./build/windows-release/bin/SparkTests          # production reader/writer + format-mirror tests
SPARK_TEST_FILE=TestFBXImporter.cpp ./build/windows-release/bin/SparkTests       # real importer, malformed input
SPARK_TEST_FILE=TestAssetValidator.cpp ./build/windows-release/bin/SparkTests
SPARK_TEST_FILE=TestAssetPipelineCache.cpp ./build/windows-release/bin/SparkTests
```

## Provenance and maintenance

Facts verified 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`) by reading
the cited sources — no full-suite or CI run at this exact tree; re-verify after
`ASSET-220`/`RDY-020` close. One-line re-checks:

```bash
# Pak format constants, reader defenses, writer
grep -n "kSparkPakMagic\|kSparkPakVersion\|kMaxTocBytes\|kMaxFileCount\|kMaxEntryBytes" SparkEngine/Source/Core/SparkPak.*

# Production-linked pak tests still present? (expect hits for Spark::SparkPakReader/Writer)
grep -n "Spark::SparkPakReader\|Spark::SparkPakWriter" Tests/TestSparkPak.cpp | head -5
# FILE*-cursor mutex + bounded TOC still in the reader
grep -n "m_fileMutex\|kMaxTocBytes" SparkEngine/Source/Core/SparkPak.cpp | head -5
# Writer still refuses symlinks during packaging
grep -n "is_symlink" SparkEngine/Source/Core/SparkPakWriter.cpp

# Path containment still rejects traversal + absolute paths
grep -n "rejected path traversal\|rejected absolute path" SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp

# Engine load path still bypasses the VFS (expect NO matches)
grep -rn "VirtualFileSystem" SparkEngine/Source/Graphics/AssetPipeline*.cpp SparkEngine/Source/Graphics/AssetTypes*.cpp SparkEngine/Source/Graphics/ModelLoading*.cpp

# .spk auto-mount wiring
grep -n "MountSparkPakArchives" SparkEngine/Source/Core/Lifecycle/GameplayLifecycleShared.cpp

# Daemon asset cache still unwired from load path (expect only DaemonLifecycle/facade matches)
grep -rn "PutAsset" --include=*.cpp SparkEngine/Source

# Two packagers still exist / consolidation status
grep -rln "GamePackager" SparkEngine/Source/Core SparkEngine/Source/Engine/Build

# Release-workflow package smoke still enforced
grep -n "PackageSmoke" .github/workflows/release.yml

# Readiness truth for this domain
grep -n '"id": "G04"' docs/site/readiness.json
grep -n '"id": "RDY-020"\|"id": "ASSET-220"' docs/readiness/work-items/*.json

# Importer/decoder limits
grep -n "kMaxDecodedBytes\|kMaxProperties" SparkEngine/Source/Graphics/FBXImporter.h
grep -n "kMaxInputBytes\|kMaxDimension\|kMaxWorkingBytes" SparkEngine/Source/Graphics/EXRLoader.h

# Compression backends vendored + defined
grep -n "SPARK_MINIZ_AVAILABLE\|SPARK_ZSTD_AVAILABLE" CMakeLists.txt && ls ThirdParty/Utils/miniz ThirdParty/Utils/zstd
```

Volatile claims most likely to change: the VFS-bypass gap (someone may wire `AssetPipeline`
into the VFS), the daemon cache staying Phase-3a-only, the duplicate `Spark::GamePackager`
surviving, the stale "smoke does not call Spark symbols" sentence in `ASSET-220`, and the
`--compress` CLI stub. Re-check each before repeating it.
