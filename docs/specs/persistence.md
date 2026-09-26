# TERRAFRONT persistence: commit, backup, restore, and recovery point

This document specifies what the TERRAFRONT account/character database (`Terrafront::TFDatabase`, `GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.h`) guarantees after a crash, how it is backed up and restored, and which local tests prove each statement. It covers `TFDatabase` only. `TFOutfitStore`, `TFSocialSystem`, and the `WorldSave` territory/progression files share the same durable commit primitive (below), but they have no backup/restore API, and no crash drill runs against them. The MMO `AsyncDatabase` key-value store is also out of scope. None of this is a hosted database service; it is one JSON file per save root.

## Commit model

- Each mutating call is one transaction under `<db>.lock` (`SavePaths::ExclusiveFileLock`). The transaction reloads and validates the committed file, applies the change, writes the whole new file, and then releases the lock. The OS drops the lock when its process dies, so a crashed writer cannot hold it.
- Each commit goes through `SavePaths::WriteDurableReplace`. It writes `<db>.tmp` in full and flushes it (`fsync` on POSIX, `FlushFileBuffers` on Windows). It then renames the staging file over `<db>`. On POSIX it also fsyncs the parent directory; on Windows the rename uses `MOVEFILE_WRITE_THROUGH`.
- The file carries `schemaVersion` and a monotonically increasing `revision`. Each character row records the revision of its last change.

## Recovery point

**After a crash, the database reopens at the last commit whose rename completed. Every call that reported success is included.** Here is what a crash at each point leaves behind:

| Crash point | On disk afterwards | Reopen result |
|-------------|--------------------|---------------|
| Before or while writing `<db>.tmp` | previous `<db>`, possibly a partial `<db>.tmp` | previous commit; the next commit unlinks the stale staging file |
| After the staging flush, before the rename (`DurableCommitStage::StagedAndSynced`) | previous `<db>`, complete `<db>.tmp` | previous commit (the call never reported success) |
| After the rename, before the directory sync (`DurableCommitStage::Renamed`) | new `<db>` | new commit on a process crash. After a power loss the rename itself may be lost. Either the complete previous file or the complete new file survives, and the call never reported success in that case |
| After the call returned `true` | new `<db>`, durable | new commit |

A process killed at any instant loses at most the one commit in flight. That commit never reported success. The database never reopens torn, empty, or half-applied: a multi-row `CommitCharacterUpdates` batch is inside one file write, so the whole batch is either present or absent.

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

When it is not known, `info.supersedesPrimaryRevision` is `false` and the restore logs a warning. This happens when no primary exists (for example, `Open` already quarantined it) or when the primary is torn and does not parse. The restored revision is then only the backup revision + 1. Later commits can reuse revision numbers, and per-row revisions, from the lost history, so a still-running authority's stale row baseline could match a restored row and overwrite it. **Operators must stop every authority on the save root before a restore and restart them after it; for an unknown displaced revision this is required, not advisory.** The restore rolls back rows, id counters, and the idempotency ledger together, so a retried operation whose effects were rolled back applies again.

A crash during the restore leaves the previous primary intact, because the restore commits through the same primitive. The same restore can then be run again.

## Rehearsal

The drill runs under the CTest labels `persistence;recovery-drill`:

```bash
ctest --test-dir build/linux-gcc-release -L recovery-drill --output-on-failure --no-tests=error
```

- `Persistence_BackupRestore_*` (7 cases, `Tests/TestDATA120BackupRestore.cpp`) cover a round trip to the backup point with the displaced primary kept, tampered or undigested backups, the schema gate (newer refused, N-1 migrated), never-overwrite and alias refusal, waiting on the authority lock, recovery of a torn primary (revision unknown, stamped backup + 1, `supersedesPrimaryRevision == false`) and of a parseable but invalid primary (stamped above its `revision`), and a running authority that must not overwrite restored rows.
- `Persistence_RecoveryDrill_*` (4 cases, POSIX only) spawn a fresh `SparkTests` process (exec, not a bare `fork()` of the multi-threaded runner) that really dies inside a commit. The child `_exit()`s at `StagedAndSynced` or at `Renamed` through the `SavePaths::DurableCommitObserver()` seam, which is null in production. Another child `_exit()`s during a restore, and a third is SIGKILLed while it commits in a loop. The parent must reopen at the recovery point above, with every acknowledged commit present, and must keep committing.

Limits of this evidence: it is local only. The Windows branch of the drill is not run (the drill is POSIX-only). Power loss is not simulated; only process death is, so the power-loss rows above rest on the fsync ordering that `Tests/Tools/test_async_database_durability.py` pins. No hosted job runs the drill on a schedule yet (the planned `recovery-drill` CI job). "Regularly rehearsed" therefore still needs recorded history.

## Source & Freshness

Written 2026-09-25 against `TFDatabase.h`, `TFDatabaseBackup.cpp`, and `TFSavePaths.h` in `GameModules/SparkGameMMOFPS/Source/Persistence/`. Tracked by work item DATA-120.
