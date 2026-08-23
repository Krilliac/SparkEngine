---
name: sparkengine-persistence-save-and-migrations
description: >-
  SparkEngine persistence runbook — save-file format, atomic writes, AsyncDatabase
  transactions, TERRAFRONT JSON stores, wire/buffer serialization primitives and
  on-disk format conventions, and save versioning/migration.
  TRIGGER when: a save file is corrupt / won't load / "save failed", changing the
  .spark_save format or bumping the save version, adding a component serializer,
  designing a length-prefixed/bounded binary format or serialization primitive,
  working on SaveSystem / QuickSave / AutoSave, AsyncDatabasePool or SQLiteConnection
  (the KV fallback store), transaction commit/rollback or shutdown flush ordering,
  TFDatabase / TFOutfitStore / terrafront_state.json durability, quarantined
  ".corrupt-*.bak" files, N-1 migration or rollback questions, parser limits on
  save/db files, or "does multiplayer persistence survive a crash?".
  DO NOT TRIGGER when: editing scene (.sscene) serialization or reflection field
  registration (use sparkengine-editor-scenes-and-reflection), network protocol,
  auth/crypto, or session security (use sparkengine-networking-security-and-multiplayer),
  or asset packaging/import integrity (use sparkengine-assets-import-and-package-integrity).
---

# SparkEngine — Persistence, Save & Migrations

Runbook for everything that writes game/player state to disk and reads it back —
including the serialization primitives and format conventions those paths use.
All paths repo-relative; all claims verified 2026-08-23 against the working tree of
branch `claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`).

**Jargon, once:**
- **Slot** — a named save file: `<saveDir>/<slotName>.spark_save`.
- **KV fallback store** — `Spark::Persistence::SQLiteConnection` is *not* SQLite; it is a
  file-based key-value store (tab-delimited text, `#!spark-kv-v2` header) that emulates a
  tiny SQL-ish command set (`SET/GET/DELETE/KEYS`).
- **Quarantine** — renaming an unreadable store file to `<path>.corrupt-<ms>.bak` and
  refusing to open, so the next eager flush cannot wipe it.
- **TERRAFRONT / TF** — the MMOFPS game module (`GameModules/SparkGameMMOFPS/`); its
  persistence is atomic-JSON files, not the engine KV store.
- **N-1 migration** — loading data written by the previous release's format.

## The four persistence layers (know which one you're in)

| Layer | Files | Format | Owner code |
|---|---|---|---|
| World saves | `<saveDir>/*.spark_save` | Custom binary, `SPRK` magic, version 1 | `SparkEngine/Source/Engine/SaveSystem/SaveSystem.{h,cpp}` |
| Engine async DB | one KV text file per `Open()` path | `#!spark-kv-v2` tab-delimited KV | `SparkEngine/Source/Engine/Persistence/AsyncDatabase.{h,cpp}` |
| TERRAFRONT stores | accounts/characters DB, outfit store, `Saves/terrafront_state.json`, territory JSON | JSON via `Spark::Json` (`SparkEngine/Source/Utils/JsonUtils.h`) | `GameModules/SparkGameMMOFPS/Source/Persistence/`, `Source/Game/TFProgressionSystemPersist.cpp`, `Source/World/TFRegionSystemNet.cpp` |
| MMO module | via AsyncDatabase (characters); accounts **in-memory only** | pseudo-SQL over KV | `GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp`, `Source/Account/MMOAccountSystem.cpp` |

There is also `FreezeSystem.h` (same directory as SaveSystem): tag-validated per-subsystem
binary snapshots with `FreezeLegacy<T>` for backward-compatible struct evolution — used by
engine diagnostics/lifecycle, tested in `Tests/TestFreezeSystem.cpp`.

## Status matrix — implemented vs tested vs CI-enforced vs release-ready

"Tested" means a named test exists in the `SparkTests` binary; those run under `ctest`
in the required CI build jobs (`.github/workflows/build.yml` runs
`ctest --test-dir build ...`), so tested ⇒ CI-enforced here unless noted.

| Claim | Status | Evidence anchor |
|---|---|---|
| `.spark_save` atomic write: tmp file + **durable flush** (`FlushFileBuffers`/`fsync`) + atomic replace (`MoveFileExW(..., REPLACE_EXISTING)` on Windows, `rename` + directory fsync on POSIX), tmp cleanup on failure | **implemented + regression-tested** (registered in SparkTests; no full-suite/CI evidence captured at this exact tree) | `SaveSystem.cpp` `FlushFileDurably` / `ReplaceFileAtomically`; `Tests/harden/Test_persistence_SaveSystem.cpp` `SaveSystem_Save_ReplacesExistingSlotAtomically` |
| Save version gate (reject v0 and v>current) | implemented + tested + CI | `Test_persistence_SaveSystem.cpp`: `SaveSystem_GetSaveMetadata_RejectsNewerVersion`, `..._RejectsVersionZeroTransactionally`, `..._RejectsBadMagic` |
| Transactional parse (truncated file never mutates `SaveData`/world) | implemented + tested + CI | `SaveSystem_Load_RejectsTruncatedCustomStateCountWithoutChangingWorld`, `..._RejectsEveryTruncatedCustomStateField` |
| Save parser budgets (size/count caps) | implemented; caps themselves not individually unit-tested | constants in `ReadFromFile`/`ReadMetadataOnly`, table below |
| AsyncDatabase shared-connection visibility + survive close/reopen | implemented + tested + CI | `Test_persistence_AsyncDatabasePool.cpp` (2 tests) |
| AsyncDatabase parameter-marker substitution (`?10` vs `?1`, value containing `?N`) | implemented + tested + CI | `Test_persistence_AsyncDatabaseParams.cpp`, `TestAsyncDatabaseRegressions.cpp` |
| AsyncDatabase transaction **crash** durability | **open** — `FlushToDisk` still truncates the live file in place (no tmp+rename); a crash mid-flush tears it | `AsyncDatabase.cpp` `SQLiteConnection::FlushToDisk` (`std::ofstream file(m_dbPath, std::ios::trunc)`) |
| AsyncDatabase close/enqueue race | **closed** — admission and worker exit now share the same mutex-protected state: `Close()` flips `m_accepting` under `m_queueMutex`, workers drain every accepted item before exiting, and post-close enqueues fail fast with "Database pool is not open" instead of stranding a future | `AsyncDatabase.cpp` `Close`/`AsyncQuery` (`m_accepting`); `Tests/harden/Test_persistence_AsyncDatabasePool.cpp` `AsyncDatabasePool_CloseRace_DrainsAcceptedAndCompletesRejectedWork` |
| TF JSON strict parse + corrupt-file quarantine | implemented + tested + CI | `TestTFOnboarding.cpp` `TFDatabase_CorruptFile_QuarantinedNotWiped`; `TestTFOutfitStore.cpp` `TFOutfitStore_CorruptFileQuarantinedNotWiped`; `TestJsonStrict.cpp` |
| TF atomic JSON writes (tmp + rename, with remove+rename fallback) | implemented; round-trip tested (`TFDatabase_AccountCharacter_RoundTrip`, `TFOutfitStore_DiskRoundTrip`); interruption not tested | `TFDatabase.cpp` `SaveToDisk`, `TFOutfitStoreDisk.cpp`, `TFRegionSystemNet.cpp` |
| N-1 save migration + rollback fixtures | **open** (release-blocking) | readiness `SAVE-230` (P0, open) in `docs/readiness/ENGINE_READINESS_HANDOFF.md` |
| Transactional/backed-up/recoverable multiplayer persistence | **open** (release-blocking) | readiness `DATA-120` (P1, open); required CI jobs `persistence-integration` / `recovery-drill` **do not exist** in `.github/workflows/` |
| MMO account passwords use stand-in hashing / mismatch on login | **closed — stale claim** | `MMOAccountSystem.cpp` uses `Spark::PasswordHash::Create/Verify` and `Spark::SecureRandom::HexToken(16)`; see reconciliation below |
| MMO accounts survive engine restart | **open** — `MMOAccountSystem` state (`m_accounts`, `m_sessions`) is in-memory only; nothing in that class touches disk | grep `Save|Load|Disk|file` over `MMOAccountSystem.{h,cpp}` finds no persistence path |

**Nothing in this domain is release-ready.** `SAVE-230` and `DATA-120` are open and
release-blocking in the readiness handoff; local persistence must never be described as
production-grade until their acceptance criteria (N-1 fixtures, rollback, forced-failure
recovery, rehearsed backup restore) have evidence.

## `.spark_save` on-disk format (version 1)

Defined entirely in `SaveSystem.cpp` (`kCurrentSaveVersion = 1`). Layout, in order:

1. 4-byte magic `"SPRK"`.
2. `uint32` format version.
3. `uint32` metadata length + newline-delimited text block:
   `saveName\n sceneName\n playerClass\n` then whitespace-separated
   `timestamp playTime playerHealth playerArmor posX posY posZ kills deaths`.
4. `uint32` entity count; per entity: `uint16`-length-prefixed name, `uint16` component
   count; per component: prefixed type name, `uint16` property count, prefixed key/value
   string pairs.
5. `uint32` customState count + prefixed key/value pairs (always present, even when 0).

Writer-side rejections (fail the save rather than corrupt it): any string > 65,535 bytes
(`rejectIfTooLong`), any embedded `\n`/`\r` in the three getline metadata fields
(`rejectIfHasNewline`), component/property counts > 65,535.

Reader-side budgets (all in `ReadFromFile` / `ReadMetadataOnly`):

| Budget | Value | Constant |
|---|---|---|
| Max file size | 512 MB | `kMaxSaveFileSize` |
| Max entities | 1,000,000 | `kMaxEntities` |
| Max customState pairs | 100,000 | `kMaxCustomState` |
| Max any string | 65,535 (uint16 prefix) | writer/reader symmetric (`kMaxPropertyLen`) |
| Max metadata block (metadata-only read) | 64 KB | `kMaxMetaSize` |
| Slot name | ≤ 64 chars, `[A-Za-z0-9_-]` only | `IsValidSlotName` (also blocks path traversal) |
| Trailing bytes | rejected (`offset != fileData.size()`) | end of `ReadFromFile` |

Reserved slots: `__quicksave`, `__autosave_0..N-1` (rotating, default 3).

## Versioning and migration — decision rules

- **Bump `kCurrentSaveVersion`** whenever the binary layout changes. The load path already
  rejects `version == 0 || version > kCurrentSaveVersion` and has a migration hook
  (`if (version < kCurrentSaveVersion)` in `ReadFromFile`) that currently only logs —
  version 1 is the baseline, so there is nothing to migrate yet. Each future bump must add
  a real upgrade branch there **and** in `ReadMetadataOnly`'s version gate.
- **Adding a field to an existing component**: no version bump needed — components are
  string-keyed property maps; deserializers use `SafeGetFloat/SafeGetUint32/SafeGetString`
  with defaults, so missing keys degrade to defaults. This is the additive-migration path.
- **Renaming or re-typing a property key is a breaking change**: old saves silently fall
  back to the default. Either keep reading the old key alongside the new one, or bump the
  version and migrate.
- **TERRAFRONT JSON policy is additive-only, with no version field at all**: new keys are
  optional on load (`HasKey` + defaults — see the `W6`/`loadout-depth` comments in
  `TFDatabase.cpp`), and persisted keys must **never be renamed**
  (`TFUnlockTree.h`: "durable unlock key (persisted; never rename)").
- **N-1 fixtures do not exist.** There are no `SaveMigration_*` / `BackupRestore` tests
  anywhere in `Tests/` (verified by grep; the atomic same-slot **overwrite** path *is* now
  covered by `SaveSystem_Save_ReplacesExistingSlotAtomically` — that is a durability test,
  not a migration fixture). Do not claim migration or rollback works; per `SAVE-230` that
  evidence is required before any compatibility claim.
- **Rollback story today**: only the TF quarantine rename (`.corrupt-<ms>.bak`) and the
  fact that a failed atomic write leaves the previous file intact. There is no
  point-in-time restore, no backup rotation, no restore rehearsal (open per `DATA-120`).

## AsyncDatabase (engine persistence layer) — how it actually works

`AsyncDatabasePool::Open(path, poolSize)` launches `poolSize` worker threads **but all
queries — async and sync — execute through one shared `SQLiteConnection` serialized by
`m_syncMutex`**. This is deliberate: the fallback store is in-memory + one file, and
per-worker connections previously caused the P0 data-loss bug (async writes invisible to
sync reads, last flush clobbering the rest — see the header comment in
`Tests/harden/Test_persistence_AsyncDatabasePool.cpp`).

Durability rules as implemented:

- Non-transactional `SET`/`DELETE` flush to disk immediately (`ExecuteRaw`, when
  `!m_inTransaction`).
- Transactions: `BeginTransaction` snapshots the whole KV map; failure of any statement
  → `RollbackTransaction` restores the snapshot (in-memory, atomic within the process);
  success → `CommitTransaction` then one `FlushToDisk`. The worker holds `m_syncMutex`
  across the entire transaction.
- `Close()` signals `m_stopping`, joins workers (they drain the queue first), then closes
  connections — `SQLiteConnection::Close()` does a final flush. Clean close/reopen
  durability is proven by `AsyncDatabasePool_AsyncAndSyncWrites_SurviveCloseReopen`.
- Prepared statements are template strings with `?0 ?1 ...` parameter markers; substitution is
  a single left-to-right pass (multi-digit indices safe, substituted values are never
  re-scanned); string params are single-quoted with `''` escaping, and the `SET` handler
  decodes one well-formed quoted literal back (loader-side inverse:
  `UnquoteStoredString` in `MMOPersistenceSystem.cpp`).

**Close/admission contract (fixed 2026-08-23; the old close/enqueue race is closed):**
admission (`AsyncQuery`/`AsyncQueryWithCallback`/`AsyncTransaction`) and `Close()` now
share the same `m_queueMutex`-protected state. `Close()` flips `m_accepting` under the
lock, so every item already accepted into `m_workQueue` is owned by the workers and
**drained before they may exit**; anything submitted after that fails fast with the
"Database pool is not open" result instead of stranding a `future.get()`. Regression:
`AsyncDatabasePool_CloseRace_DrainsAcceptedAndCompletesRejectedWork`
(`Tests/harden/Test_persistence_AsyncDatabasePool.cpp`).

**Known-open hazards (current code, do not paper over):**

1. **Torn flush**: `FlushToDisk` opens `m_dbPath` with `std::ios::trunc` and writes in
   place. A crash or power loss mid-flush leaves a truncated KV file — and `LoadFromDisk`
   tolerantly skips malformed lines rather than quarantining, so damage is silent partial
   data loss. Fix shape: tmp + rename like `TFDatabase::SaveToDisk` (or the SaveSystem
   `FlushFileDurably` + `ReplaceFileAtomically` pair). Until fixed, do not claim crash
   durability for this layer.
2. `ProcessCallbacks()` must be pumped every frame by the owner (`MMOPersistenceSystem::
   Update` does), or callback results queue up forever.

## TERRAFRONT JSON durability contract

Every TF store follows the same three-part contract (copy it for new stores):

1. **Strict parse on load** — `Spark::Json::ParseStrict` (rejects truncation/trailing
   junk deterministically; the lenient `Parse` is backend-dependent and once accepted
   `'{ this is garbage'` as an object — background in `TestJsonStrict.cpp`).
2. **Quarantine on corrupt open** — rename to `<path>.corrupt-<ms>.bak`, log, return
   false, never fall through to an empty in-memory DB (which would wipe the file on the
   next eager flush). See `TFDatabase::Open` and `TFOutfitStore` open path; callers must
   not retry-loop a refused open (`TFOutfitSystemServer.cpp` `m_storeOpenFailed`).
3. **Atomic eager save** — every mutator calls `SaveToDisk()` (tmp + `fs::rename`; on
   rename failure a remove+rename fallback trades atomicity for success). `Close()` is a
   safety net, not the durability mechanism. `TFOutfitStore` adds a debounced flush
   (tested: `TFOutfitStore_DebouncedFlush`).

Shared file caveat: `Saves/terrafront_state.json` holds both progression and territory
keys; each writer must read-modify-write **only its own key** (documented in
`TFProgressionSystem.h`).

## Parser budgets — where they exist and where they don't

- `.spark_save`: full budget table above; bounded-cursor reads (`readBytes` checks every
  length against remaining bytes), exact-length check at EOF. Good model to copy.
- `Spark::Json` (`SparkEngine/Source/Utils/JsonUtils.h`): recursive-descent
  (`ParseValue` → `ParseObject`/`ParseArray` → `ParseValue`) with **no nesting-depth
  cap** (verified: no depth counter anywhere in the parser). A hostile deeply-nested
  file can overflow the stack. **Open** — treat TF store files as trusted local files
  only; add a depth budget before parsing anything network-supplied with it.
- KV fallback store: line-based; no per-line or file-size cap (**open**, low risk while
  the file is locally owned).

## Failure modes → first moves

| Symptom | Likely cause | First move |
|---|---|---|
| `Save()` returns false, old file intact | writer rejection (string >64K, newline in metadata) or tmp-file I/O error | check log for `refusing to truncate` / `refusing to write a corrupt save` / `rename failed`; old save is still valid |
| `Load()` false on a file that "should work" | version gate, bad magic, truncation, trailing bytes | log names the exact reason + offset; compare version against `kCurrentSaveVersion` |
| Save slot missing from the slot list | `ReadMetadataOnly` version/magic gate, or metadata block > 64 KB | `save info <slot>` in the console (`SaveSystem::Console_GetSaveInfo`) |
| `future.get()` on a DB query never returns | nobody opened the pool, or a regression of the linearized close/admission contract | pool-closed queries fail fast with "Database pool is not open"; re-run `AsyncDatabasePool_CloseRace_DrainsAcceptedAndCompletesRejectedWork` |
| DB callbacks never fire | `ProcessCallbacks()` not pumped | wire it into the owner's `Update()` |
| `[TF] db open refused ... backed up to ...` | corrupt/truncated JSON quarantined | inspect the `.corrupt-*.bak`; restore manually if salvageable; **this is working as designed**, don't "fix" it by loosening the parse |
| KV file has literal `\t`/`\n` text in values | legacy pre-`#!spark-kv-v2` file loaded raw (by design) or escaping bug | see `EscapeKVField`/`UnescapeKVField` + `AsyncDatabase_LegacyKVFile_LoadsRawBackslashes` |
| MMO accounts gone after restart | expected — accounts are in-memory only (open item) | do not report as a regression; it is the documented gap |

## Verification commands

From repo root. Build first if needed:

```bash
cmake --preset windows-release        # or linux-gcc-release
cmake --build --preset windows-release
```

Run just the persistence tests (`SPARK_TEST_FILE` filters by source file; the runner
exits 0 when the filter matches nothing, so **always confirm the ran-count is non-zero**
in the `Running N tests` / `[ RUN ]` output):

```bash
# Preset builds put the binary in build/<preset>/bin/ (check bin/<Config> too under the
# VS generator). Full selector list: sparkengine-validation-and-qa §3.
SPARK_TEST_FILE=Test_persistence ./build/windows-release/bin/SparkTests   # both harden DB/save files
SPARK_TEST_FILE=TestAsyncDatabase ./build/windows-release/bin/SparkTests  # unit + regression
SPARK_TEST_FILE=TestTFOnboarding ./build/windows-release/bin/SparkTests   # TFDatabase round-trip + quarantine
SPARK_TEST_FILE=TestTFOutfitStore ./build/windows-release/bin/SparkTests
SPARK_TEST_FILE=TestJsonStrict ./build/windows-release/bin/SparkTests
```

Note: `ctest` exposes only one test (`SparkEngineTests`, the whole binary — verified:
single `add_test` in `Tests/CMakeLists.txt`). The readiness handoff's
`ctest -L persistence` / `-R BackupRestore` selectors match **nothing today**; running
them "passing" is the classic check-that-stopped-checking. Use the env-filtered binary
above instead.

Re-verify the code claims in this skill:

```bash
grep -n "kCurrentSaveVersion" SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp
grep -n "kMaxSaveFileSize\|kMaxEntities\|kMaxCustomState\|kMaxMetaSize" SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp
grep -n "ios::trunc" SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp   # torn-flush hazard still present if this hits FlushToDisk
grep -rn "corrupt-" GameModules/SparkGameMMOFPS/Source/Persistence/
grep -rn "persistence-integration\|recovery-drill\|BackupRestore" .github/workflows/ Tests/   # empty = DATA-120 evidence still absent
```

## Reconciliation of prior claims (2026-08-23)

- **Closed (stale):** "MMO account system uses stand-in password hashing / login
  mismatch." Current `MMOAccountSystem.cpp` calls `Spark::PasswordHash::Create` (empty
  result treated as failure), `Spark::PasswordHash::Verify`, CSPRNG session tokens with
  collision retry, and failed-login lockout. Do not re-report it.
- **Closed (fixed in the current tree, regression-tested):** the AsyncDatabase
  close/enqueue strand (admission and worker exit now linearized; accepted work is
  drained, late work fails fast), and the Windows/POSIX same-slot save overwrite (durable
  flush + atomic replace, `SaveSystem_Save_ReplacesExistingSlotAtomically`). Evidence is
  registered regression tests read from the working tree — no full-suite/CI run has been
  captured at this exact tree; do not cite CI evidence for these until one exists.
- **Still open (code-confirmed):** AsyncDatabase torn flush (`FlushToDisk` trunc-in-place),
  JSON parser depth budget, N-1 migration/rollback fixtures, backup/restore drills, MMO
  account disk persistence. Readiness items `SAVE-230` (P0) and `DATA-120` (P1) remain
  open and release-blocking; the `persistence-integration` and `recovery-drill` CI jobs
  they require do not exist.

## Sibling routing

| Task | Use instead |
|---|---|
| Scene files, editor serialization, reflection field registration (`TypeRegistry`, `ComponentFactory` internals) | `sparkengine-editor-scenes-and-reflection` |
| Network protocol, auth flows, crypto quality, session security | `sparkengine-networking-security-and-multiplayer` |
| Asset packaging, importer integrity, CPack | `sparkengine-assets-import-and-package-integrity` |
| Running/adding tests, CI gates in general | `sparkengine-validation-and-qa` |
| Wire/buffer serialization primitives and format conventions | **this skill** (anchors: the `.spark_save` bounded-cursor reader, `Utils/JsonUtils.h`, `FreezeSystem.h`; network *wire* schemas belong to `sparkengine-networking-security-and-multiplayer`) |
| Root-causing a persistence regression historically | `sparkengine-failure-archaeology` |

## Provenance and maintenance

Authored 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`), by reading
the cited sources and tests — no full-suite or CI run at this exact tree. Volatile facts
and one-line re-checks:

- Save format version (currently 1): `grep -n kCurrentSaveVersion SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp`
- Durable atomic slot replace still present: `grep -n "FlushFileDurably\|ReplaceFileAtomically" SparkEngine/Source/Engine/SaveSystem/SaveSystem.cpp` and `grep -n "ReplacesExistingSlotAtomically" Tests/harden/Test_persistence_SaveSystem.cpp`
- Torn-flush hazard still open: `grep -n "ios::trunc" SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp` (hit inside `FlushToDisk` = still open)
- Close/admission still linearized: `grep -n "m_accepting" SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp` and `grep -n "CloseRace_Drains" Tests/harden/Test_persistence_AsyncDatabasePool.cpp`
- JSON depth cap: `grep -n "depth" SparkEngine/Source/Utils/JsonUtils.h` (only pretty-printer hits = still no parse cap)
- Persistence test inventory: `grep -rln "TEST(" Tests/harden/Test_persistence_*.cpp Tests/TestTFOnboarding.cpp Tests/TestTFOutfitStore.cpp Tests/TestJsonStrict.cpp`
- Readiness status: `grep -n "SAVE-230\|DATA-120" docs/readiness/ENGINE_READINESS_HANDOFF.md`
- Required-but-missing CI jobs: `grep -rn "persistence-integration\|recovery-drill" .github/workflows/`
