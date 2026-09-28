# TERRAFRONT persistence: commit, backup, restore, and recovery point

This document specifies what the TERRAFRONT account/character database (`Terrafront::TFDatabase`, `GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.h`) guarantees after a crash, how it is backed up and restored, and which local tests prove each statement. Its backup and restore sections cover `TFDatabase` only. `TFOutfitStore`, `TFSocialSystem`, and the `WorldSave` territory/progression files share the same durable commit primitive (below) and the same recovery point, and a crash drill runs against each of them, but they have no backup/restore API. The MMO `AsyncDatabase` key-value store is also out of scope. None of this is a hosted database service; it is one JSON file per save root.

## Commit model

- Each mutating call is one transaction under `<db>.lock` (`SavePaths::ExclusiveFileLock`). The transaction reloads and validates the committed file, applies the change, writes the whole new file, and then releases the lock. The OS drops the lock when its process dies, so a crashed writer cannot hold it.
- Each commit goes through `SavePaths::WriteDurableReplace`. It writes `<db>.tmp` in full and flushes it (`fsync` on POSIX, `FlushFileBuffers` on Windows). It then renames the staging file over `<db>`. On POSIX it also fsyncs the parent directory; on Windows the rename uses `MOVEFILE_WRITE_THROUGH`.
- The file carries `schemaVersion` and a monotonically increasing `revision`. Each character row records the revision of its last change.
- This build writes schema v4, which adds each character's `resident` continent key (TF-120, below). v2 files load with every character out of world and are rewritten as v4. Schema v3 is retired: for a short time, `b2d2953` wrote v3 files, v2 content plus an `appliedOperations` ledger that no caller ever filled. A v3 file whose ledger is an empty array loads as v2 content and is rewritten as v4. Any other v3 file fails closed as `UnsupportedVersion`, because rewriting it would drop recorded operation ids. A v4 file is `UnsupportedVersion` to a v2 build, so a rollback cannot drop residency.

## Character residency (TF-120)

A character is in world on at most one continent authority sharing the save root.

- Each authority process binds its `TFDatabase` to its continent (`BindAuthority`, called by `TFServerSim::EnsureAuthorityDatabaseOpen`). The bind holds `<db>.authority.<continent>.lock` for the life of the instance. A second live authority for the same continent fails the bind with `AuthorityHeld`, and the server then serves no login. Holding the lock proves every earlier authority for that continent is dead, so the bind clears the residency they left in one transaction.
- Enter world (`TFCharacterSystem::EnterWorld`) claims the character in one transaction. The claim is refused with `ResidentElsewhere` and writes nothing while the character is resident on another continent whose authority lock is held. If that lock is free, the owner is dead and the claim takes the character over from its last commit. The claim stamps a new row revision, so an authority that was fenced out holds a stale baseline. On a bound instance every absolute character write also requires the row to be resident on the bound continent.
- Leave world (`TFCharacterSystem::LeaveWorld`) releases the residency only after the character's final progress and meta are durable. A disconnect whose meta flush fails parks the meta, and the character stays resident until the progression sweep commits (or discards) the parked row; `TFProgressionSystem::SaveNow` then releases it. A lost progress flush leaves the character resident on this continent, where it can still re-enter, until the continent's next bind.
- Tests: `TF120_Residency_*` (`Tests/TestTF120Residency.cpp`).

## Concurrent writers

Several authority processes may share one save root. No store duplicates or loses state when two of them write at once:

- **Character database (characters and the flux economy).** Multi-writer. Every call is a transaction under `<db>.lock` against a freshly reloaded file, so creates allocate ids and check the unique account and character names against every authority's rows: two contested creates of one name commit exactly once. Absolute character writes (progress, meta, `CommitCharacterUpdates`) are optimistic. A row that changed since the writer's baseline is refused with `Conflict` and nothing is written, so two authorities that both read a 10-flux wallet cannot both spend it. Retries are guarded by these absolute-value revision checks and by unique names, not by an operation-id ledger.
- **World files (`terrafront_territory.<continent>.json`, `terrafront_state.<continent>.json`).** Single-writer. `WorldSave::WriteJson` takes the caller's `SavePaths::ExclusiveFileLock` and refuses, before any I/O, unless that lease is held on exactly the destination; there is no leaseless overload. `TFRegionSystem::LoadPersisted` and `TFProgressionSystem::LoadFromDisk` take the lease before they read or migrate anything. A second authority for the same continent fails to take it, latches its writes off, and its checkpoints report failure, so it can never report a successful save. The lease is held until `Shutdown` after the final checkpoint, and the OS releases it when the process dies. These files are guarded independently of the continent bind above, because territory loads at initialization, before the database binds lazily on the first login.
- **Outfit and social stores.** Single-writer behind a lifetime lock taken at open (`TFOutfitStore_ExclusivePersistenceLockRejectsSecondAuthority`).
- **MMO `AsyncDatabase` store.** A single worker applies writes in FIFO order, and the `SQLiteConnection` authority lock refuses a second authority (`SEC2Persist_AsyncDatabaseRejectsSecondAuthority`).
- Tests: `Persistence_Concurrency_*` (4 cases, `Tests/TestDATA120PersistenceReal.cpp`) cover the lease check on world-file writes, a refused second world-file writer and the handover to it, a contested create and a stale double spend. `TF120_SharedRoot_*` (`Tests/TestTF120SharedSaveRoot.cpp`) interleaves spawned authority processes on one database with no lost flux, xp or unlock writes. Its spawned-peer cases are POSIX-only, so Windows runs only the in-process cases. `Persistence_Transaction_*` covers atomic unlock purchases and currency transfers.

## Schema versions of the other stores

Every store reads schema N and N-1, writes N, and fails closed on anything newer, so an older binary never loads and rewrites a file a newer build wrote (a rollback would silently drop the newer fields).

| Store | Current | N-1 | Newer file | Fixtures |
|-------|---------|-----|------------|----------|
| Territory, `terrafront_territory.<continent>.json` | `version` 1 (`WorldSave::kTerritorySchemaVersion`) | v0, no `version`: loaded, skyanchor owners coerced home, missing `dominion` inactive, rewritten as v1 | `WorldSave::TerritoryDecodeResult::NewerSchema`; writes latch off and the file is never rewritten | `Persistence_Migration_Territory*` (`Tests/TestDATA120PersistenceReal.cpp`) |
| Outfits, `outfits.json` | `schemaVersion` 1 (`TFOutfitStore::kSchemaVersion`) | v0, no key: loaded, rewritten as v1 by the next save | `Open` returns false, the instance latches, the ownership lock is released, no `.corrupt-*.bak` copy is made and the file stays byte-identical | `Persistence_Migration_Outfit*` (`Tests/TestTFOutfitStore.cpp`) |
| Social, `terrafront_social.json` | `schemaVersion` 1 (`TFSocialSystem::kStoreSchemaVersion`) | v0, `{"characters": [...]}` only: loaded, rewritten as v1 by the next save | refused before the exact-key check, store writes latch off, no quarantine copy, file byte-identical | `Persistence_Migration_Social*` (`Tests/TestTFSocialStore.cpp`) |

A `schemaVersion` of 0 or one that is not a positive integer is corruption in both JSON stores, not a legacy file.

The territory decode (`WorldSave::DecodeTerritory` in `TFWorldSave.h`) is pure, so the fixtures run it without a game context. `TFRegionSystem::LoadPersisted` keeps the file I/O, the logging and the latch policy.

The progression world file (`terrafront_state.<continent>.json`, key `progression`) carries no schema version on purpose. Its rows are keyed by session-scoped `PlayerId`s, and the durable per-character progression lives in `TFDatabase`, which is versioned above. Nothing restores a character from the world file.

## Recovery point

**After a crash, the database reopens at the last commit whose rename completed. Every call that reported success is included.** Here is what a crash at each point leaves behind:

| Crash point | On disk afterwards | Reopen result |
|-------------|--------------------|---------------|
| Before or while writing `<db>.tmp` | previous `<db>`, possibly a partial `<db>.tmp` | previous commit; the next commit unlinks the stale staging file |
| After the staging flush, before the rename (`DurableCommitStage::StagedAndSynced`) | previous `<db>`, complete `<db>.tmp` | previous commit (the call never reported success) |
| After the rename, before the directory sync (`DurableCommitStage::Renamed`) | new `<db>` | new commit on a process crash. After a power loss the rename itself may be lost. Either the complete previous file or the complete new file survives, and the call never reported success in that case |
| After the call returned `true` | new `<db>`, durable | new commit |

A process killed at any instant loses at most the one commit in flight. That commit never reported success. The database never reopens torn, empty, or half-applied: a multi-row `CommitCharacterUpdates` batch is inside one file write, so the whole batch is either present or absent.

The other stores commit whole files through the same primitive, so they reopen at the same point:

| Store | Crash before the rename | Crash after the rename | Lock afterwards | Drill |
|-------|-------------------------|------------------------|-----------------|-------|
| `TFOutfitStore` (`outfits.json`) | previous roster, stale `.tmp` replaced by the next save | new roster | lifetime lock released by the OS; the next `Open` succeeds | `Persistence_RecoveryDrill_Outfit*` |
| `TFSocialSystem` store (`terrafront_social.json`) | previous document byte for byte | new document | not held by the drilled serializer (the system takes it at `Initialize`) | `Persistence_RecoveryDrill_Social*` |
| `WorldSave` territory file | not drilled separately (same primitive, same staging path) | the new region owners load on restart | writer lease released by the OS; the restarted authority takes it and writes | `Persistence_RecoveryDrill_TerritoryWriteCrashAfterRename` |

If the reopened file fails validation, `Open` quarantines it as `<db>.corrupt-<ms>.bak` and latches off. If the primary is missing while such a quarantine file exists, `Open` refuses to start empty. Both cases are recovered by restoring a backup (below).

## Backup

`TFDatabase::CreateBackup(dbPath, backupPath, info)`:

1. It takes the authority lock on `dbPath`, so the copy is exactly one committed revision, and it can run while authorities are live.
2. It reads the committed bytes and validates them with the same rules `Open` uses. A corrupt primary is refused, and so is a primary from a newer schema.
3. It writes `backupPath` durably with the exact committed bytes. It then writes `BackupDigestPath(backupPath)` (`<backup>.sha256`), a `sha256sum`-format line (`<hex>  <name>`), so `sha256sum -c` can also check the pair.
4. It re-reads both files and verifies the digest. If verification fails, it removes both files and does not return `Ok`.

It never overwrites an existing backup or sidecar. It refuses a target that aliases the database, its `.tmp` staging file, or its `.lock` file.

## Restore

`TFDatabase::RestoreFromBackup(backupPath, dbPath, info)` checks the backup before it touches the primary:

1. The backup bytes must match the sidecar digest. A missing or malformed sidecar is `DigestMissing`, and a mismatch is `DigestMismatch`.
2. The bytes must pass `Open`'s full validation. A newer-schema backup is `BackupUnsupportedVersion` and is never loaded or rewritten. An older-schema (N-1) backup is migrated, because the restore writes it back in the current schema.
3. Under the authority lock, it durably copies the current primary to `<db>.pre-restore-<ms>.bak`, whatever state the primary is in, so the restore can be reversed.
4. It writes the restored rows with `revision = max(backup revision, displaced primary revision) + 1`. The displaced revision comes from the loaded file, or from its `revision` key when the file parses as JSON but does not validate. It then re-reads and validates the result.

When the displaced revision is known, `info.supersedesPrimaryRevision` is `true` and revisions do not repeat across the restore. An authority still running with a baseline taken after the backup point gets `TFDatabaseStatus::Conflict` on its next absolute write, so it cannot overwrite the restored rows. It must re-acquire the character. An authority that reads a revision older than one it already saw fails closed.

When it is not known, `info.supersedesPrimaryRevision` is `false` and the restore logs a warning. This happens when no primary exists (for example, `Open` already quarantined it) or when the primary is torn and does not parse. The restored revision is then only the backup revision + 1. Later commits can reuse revision numbers, and per-row revisions, from the lost history, so a still-running authority's stale row baseline could match a restored row and overwrite it. **Operators must stop every authority on the save root before a restore and restart them after it; for an unknown displaced revision this is required, not advisory.** The restore rolls back rows and id counters together. There is no operation-id ledger, so nothing recognises a retried operation; a retry is guarded only by the per-row revision check.

A crash during the restore leaves the previous primary intact, because the restore commits through the same primitive. The same restore can then be run again.

## Rehearsal

The drill runs under the CTest labels `persistence;recovery-drill`:

```bash
ctest --test-dir build/linux-gcc-release -L recovery-drill --output-on-failure --no-tests=error
```

- `Persistence_BackupRestore_*` (7 cases, `Tests/TestDATA120BackupRestore.cpp`) cover a round trip to the backup point with the displaced primary kept, tampered or undigested backups, the schema gate (newer refused, N-1 migrated), never-overwrite and alias refusal, waiting on the authority lock, recovery of a torn primary (revision unknown, stamped backup + 1, `supersedesPrimaryRevision == false`) and of a parseable but invalid primary (stamped above its `revision`), and a running authority that must not overwrite restored rows.
- `Persistence_RecoveryDrill_*` (9 cases, on Windows and POSIX) spawn a fresh `SparkTests` process (exec, not a bare `fork()` of the multi-threaded runner) that really dies inside a commit. The child `_exit()`s at `StagedAndSynced` or at `Renamed` through the `SavePaths::DurableCommitObserver()` seam, which is null in production. Another child `_exit()`s during a restore, and a third is killed (SIGKILL on POSIX, `TerminateProcess` on Windows) while it commits in a loop. Five more crash the outfit store, the social store and a territory write (table above). The parent must reopen at the recovery point above, with every acknowledged commit present, and must keep committing. On POSIX the child's environment is set through `env(1)`; on Windows it inherits a scoped copy of the parent's environment (the CTest runs `RUN_SERIAL`). Both branches exercise their own `WriteDurableReplace` and `ExclusiveFileLock` code.

Limits of this evidence: it is local only. Power loss is not simulated; a fault-injecting filesystem (LazyFS, dm-flakey) is not available on these hosts, and the criterion names process and database failure. Only process death is simulated, so the power-loss rows above rest on the fsync ordering that `Tests/Tools/test_async_database_durability.py` pins. No hosted job runs the drill on a schedule yet (the planned `recovery-drill` CI job). "Regularly rehearsed" therefore still needs recorded history.

## Source & Freshness

Written 2026-09-25 against `TFDatabase.h`, `TFDatabaseBackup.cpp`, and `TFSavePaths.h` in `GameModules/SparkGameMMOFPS/Source/Persistence/`. Updated 2026-09-27 for the store schema versions, the other stores' recovery drills and the Windows drill. Tracked by work item DATA-120.
