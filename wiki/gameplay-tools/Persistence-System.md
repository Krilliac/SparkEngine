# Persistence System

SparkEngine provides two independent persistence layers for different use cases:

1. **[Save System](Save-System.md)** -- ECS-aware game state serialization to compressed JSON files. Designed for single-player save slots, quicksave/quickload, and autosave rotation.
2. **AsyncDatabase** (this page) -- TrinityCore-inspired async database layer with connection pooling, prepared statements, and transaction support. Designed for server-side and multiplayer persistence.

**Source:** `SparkEngine/Source/Engine/Persistence/AsyncDatabase.h`, `AsyncDatabase.cpp`

**Namespace:** `Spark::Persistence`

## Overview

AsyncDatabase is modeled after TrinityCore's `DatabaseWorkerPool`. It provides:

- A thread pool where each worker owns a dedicated database connection
- Prepared statements registered by ID and replicated to all connections
- Async queries via `std::future` or main-thread callbacks
- Atomic transaction batching (all-or-nothing)
- A synchronous query path for blocking operations

The default backend is a file-based key-value store (`SQLiteConnection`) that requires no external libraries. The `IDatabaseConnection` interface is designed to be swapped for real SQLite, MySQL, or PostgreSQL backends without changing calling code.

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                         Game / Server Code                            │
│      AsyncQuery() / AsyncQueryWithCallback() / SyncQuery()            │
├──────────────────────────────────────────────────────────────────────┤
│                        AsyncDatabasePool                              │
│                                                                       │
│   ┌─────────────────────────┐   ┌──────────────────────────────────┐ │
│   │  Work Queue (mutex)      │   │  Callback Queue (mutex)          │ │
│   │  WorkItem { stmtId,     │   │  CompletedCallback { callback,   │ │
│   │    params, promise,     │   │    result }                      │ │
│   │    callback }           │   │                                   │ │
│   └────────────┬────────────┘   └──────────────┬───────────────────┘ │
│                │                                │                     │
│   ┌────────────▼────────────┐   ProcessCallbacks() dispatches on     │
│   │  Worker Threads (N)     │   main thread each frame               │
│   │  ┌───────────────────┐  │                                        │
│   │  │ IDatabaseConnection│  │                                        │
│   │  │ (1 per thread)    │  │                                        │
│   │  └───────────────────┘  │                                        │
│   └─────────────────────────┘                                        │
├──────────────────────────────────────────────────────────────────────┤
│                     IDatabaseConnection                               │
│               (abstract: Open, Close, Execute,                        │
│                PrepareStatement, BeginTransaction)                    │
├──────────────────────────────────────────────────────────────────────┤
│                      SQLiteConnection                                 │
│           (file-based key-value fallback store)                       │
└──────────────────────────────────────────────────────────────────────┘
```

## Key Classes

### QueryValue

A variant representing a single column value from a query result:

```cpp
using QueryValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;
```

`std::monostate` represents SQL NULL.

### QueryRow

A single row of query results with typed accessors:

| Method | Description |
|--------|-------------|
| `GetInt(col)` | Get column as `int64_t`. Throws `std::bad_variant_access` if wrong type. |
| `GetDouble(col)` | Get column as `double`. Throws `std::bad_variant_access` if wrong type. |
| `GetString(col)` | Get column as `const std::string&`. Throws `std::bad_variant_access` if wrong type. |
| `IsNull(col)` | Returns true if the column holds `monostate` (NULL) or is out of range. |

### QueryResult

Result of a database query or transaction:

```cpp
struct QueryResult
{
    bool success = false;
    std::string errorMessage;
    std::vector<QueryRow> rows;
    int affectedRows = 0;
    int64_t lastInsertId = 0;

    bool HasRows() const;
    size_t RowCount() const;
};
```

| Field | Description |
|-------|-------------|
| `success` | Whether the query completed without error |
| `errorMessage` | Error description if `success` is false |
| `rows` | Result rows for SELECT-type queries |
| `affectedRows` | Number of rows affected by INSERT/UPDATE/DELETE |
| `lastInsertId` | Auto-increment ID of the last inserted row |

### PreparedStatementData

Handle for binding parameters to a prepared statement before execution:

```cpp
struct PreparedStatementData
{
    PreparedStatementID id = 0;
    std::vector<PreparedStatementParam> params;

    void SetInt(size_t index, int64_t value);
    void SetDouble(size_t index, double value);
    void SetString(size_t index, const std::string& value);
    void SetNull(size_t index);
    void ClearParams();
};
```

Parameters auto-resize to accommodate the given index. Call `ClearParams()` to reuse the statement with new bindings.

### Transaction

A batch of queries executed atomically (all-or-nothing):

```cpp
struct Transaction
{
    std::vector<std::pair<PreparedStatementID, std::vector<PreparedStatementParam>>> queries;

    void Append(PreparedStatementID stmtId, std::vector<PreparedStatementParam> params = {});
    size_t Size() const;
};
```

On execution, the pool wraps the batch in `BEGIN` / `COMMIT`. If any query fails, the entire transaction is rolled back.

### IDatabaseConnection

Abstract interface for a single database connection. Implement this to add a new database backend:

| Method | Description |
|--------|-------------|
| `Open(connectionString)` | Open the connection. Returns true on success. |
| `Close()` | Close the connection and release resources. |
| `IsOpen()` | Check whether the connection is currently open. |
| `PrepareStatement(id, sql)` | Register a prepared statement SQL string by ID. |
| `Execute(id, params)` | Execute a prepared statement with bound parameters. |
| `ExecuteRaw(sql)` | Execute a raw SQL string (no parameter binding). |
| `BeginTransaction()` | Start a transaction. |
| `CommitTransaction()` | Commit the current transaction. |
| `RollbackTransaction()` | Roll back the current transaction. |

### SQLiteConnection

File-based key-value fallback implementing `IDatabaseConnection`. This is **not** a real SQLite binding -- it stores data as tab-separated key-value pairs on disk and supports a minimal command set:

| Command | Syntax | Description |
|---------|--------|-------------|
| `SET` | `SET key value` | Store a key-value pair |
| `GET` | `GET key` | Retrieve a value by key |
| `DELETE` | `DELETE key` | Remove a key-value pair |
| `KEYS` | `KEYS [prefix]` | List all keys, optionally filtered by prefix |

Parameters in prepared statements use `?0`, `?1`, `?2` placeholders, substituted by string replacement before execution. Transaction support uses an in-memory snapshot for rollback.

The `SQLiteConnection` is intended as a zero-dependency fallback. For production use, replace it with a real SQLite, MySQL, or PostgreSQL implementation of `IDatabaseConnection`.

### AsyncDatabasePool

Thread-pool based async database with connection pooling. This is the primary API surface for game and server code.

## AsyncDatabasePool API

| Method | Description |
|--------|-------------|
| `Open(connectionString, poolSize)` | Create `poolSize` worker threads, each with its own database connection. Returns true if all connections opened successfully. Minimum pool size is 1. |
| `Close()` | Signal all workers to stop, join threads, close all connections. |
| `PrepareStatement(id, sql)` | Register a prepared statement by ID. Replicates to all live connections. Call during initialization before issuing queries. |
| `AsyncQuery(id, params)` | Enqueue a query and return a `std::future<QueryResult>`. |
| `AsyncQueryWithCallback(id, params, callback)` | Enqueue a query with a completion callback. The callback is dispatched on the main thread via `ProcessCallbacks()`. |
| `AsyncTransaction(transaction)` | Execute a `Transaction` atomically and asynchronously. Returns a `std::future<QueryResult>`. |
| `SyncQuery(id, params)` | Execute a query synchronously on the calling thread. Uses connection[0] guarded by a dedicated mutex. Blocks the caller. |
| `ProcessCallbacks()` | Dispatch completed async callbacks on the calling (main) thread. Call once per frame from the main loop. |
| `IsOpen()` | Check whether the pool is open (atomic). |
| `GetPendingQueryCount()` | Number of queries currently in the work queue (atomic). |
| `GetPoolSize()` | Number of worker threads. |

## Thread Safety

| Mechanism | Protection | Notes |
|-----------|-----------|-------|
| Work queue (`m_workQueue`) | `std::mutex` + `std::condition_variable` | Workers wait on the CV; producers notify on enqueue |
| Callback results (`m_completedCallbacks`) | `std::mutex` | Workers push; main thread swaps and dispatches in `ProcessCallbacks()` |
| Sync query (`m_syncConnection`) | `std::mutex` (`m_syncMutex`) | Guards connection[0] for `SyncQuery()` calls from the main thread |
| Pending count (`m_pendingCount`) | `std::atomic<int>` | Incremented on enqueue, decremented after execution |
| Open/stopping flags | `std::atomic<bool>` | Lock-free read from any thread |

**Key rule:** `ProcessCallbacks()` must be called on the main thread each frame. Callback functions execute on the main thread, so they can safely access game state without additional synchronization.

## Usage Pattern

### Initialization

```cpp
using namespace Spark::Persistence;

AsyncDatabasePool dbPool;

// Open with 2 worker threads
dbPool.Open("data/server.db", 2);

// Register prepared statements by ID
constexpr PreparedStatementID STMT_LOAD_CHARACTER = 1;
constexpr PreparedStatementID STMT_SAVE_CHARACTER = 2;
constexpr PreparedStatementID STMT_DELETE_CHARACTER = 3;

dbPool.PrepareStatement(STMT_LOAD_CHARACTER, "GET character_?0");
dbPool.PrepareStatement(STMT_SAVE_CHARACTER, "SET character_?0 ?1");
dbPool.PrepareStatement(STMT_DELETE_CHARACTER, "DELETE character_?0");
```

### Async Query with Future

```cpp
// Build parameters
std::vector<PreparedStatementParam> params;
params.push_back({int64_t{42}});  // character ID

// Fire async query
auto future = dbPool.AsyncQuery(STMT_LOAD_CHARACTER, std::move(params));

// ... later, check the result ...
if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
{
    QueryResult result = future.get();
    if (result.success && result.HasRows())
    {
        const std::string& data = result.rows[0].GetString(0);
        // Deserialize character from data
    }
}
```

### Async Query with Callback

```cpp
std::vector<PreparedStatementParam> params;
params.push_back({int64_t{42}});

dbPool.AsyncQueryWithCallback(STMT_LOAD_CHARACTER, std::move(params),
    [](QueryResult result)
    {
        // This runs on the main thread via ProcessCallbacks()
        if (result.success && result.HasRows())
        {
            const std::string& data = result.rows[0].GetString(0);
            // Safe to access game state here
        }
    });
```

### Transactions

```cpp
Transaction txn;

// Save multiple character fields atomically
std::vector<PreparedStatementParam> nameParams;
nameParams.push_back({std::string{"42"}});
nameParams.push_back({std::string{"{\"name\":\"Hero\",\"level\":10}"}});
txn.Append(STMT_SAVE_CHARACTER, std::move(nameParams));

std::vector<PreparedStatementParam> inventoryParams;
inventoryParams.push_back({std::string{"inv_42"}});
inventoryParams.push_back({std::string{"{\"slots\":[...]}"}});
txn.Append(STMT_SAVE_CHARACTER, std::move(inventoryParams));

// Execute atomically -- if any query fails, all are rolled back
auto future = dbPool.AsyncTransaction(std::move(txn));
```

### Main Loop Integration

```cpp
// In the main game/server loop
void GameLoop()
{
    // ... update game state ...

    // Dispatch completed database callbacks on the main thread
    dbPool.ProcessCallbacks();
}
```

### Shutdown

```cpp
// On shutdown, close the pool (joins all worker threads)
dbPool.Close();
```

## Relationship with SaveSystem

AsyncDatabase and [SaveSystem](Save-System.md) are **independent** persistence layers serving different purposes:

| Aspect | SaveSystem | AsyncDatabase |
|--------|-----------|---------------|
| Purpose | Single-player save/load | Server-side / multiplayer persistence |
| Data model | ECS world snapshots | Arbitrary key-value or relational data |
| I/O model | Synchronous file I/O | Async thread pool with futures/callbacks |
| Format | Compressed JSON (miniz) | Backend-dependent (file-based KV store by default) |
| Thread safety | Main thread only | Thread-safe work queue; callbacks on main thread |
| Integration | Fully wired into engine | Available but not wired into engine startup |

Neither system depends on or communicates with the other. A game can use both simultaneously -- for example, SaveSystem for local save files and AsyncDatabase for leaderboards or server-side character storage.

> **Note:** AsyncDatabase is currently available but not initialized at engine startup. To use it, create and manage an `AsyncDatabasePool` instance in your game or server code directly.

## Source Files

| File | Lines | Description |
|------|-------|-------------|
| `SparkEngine/Source/Engine/Persistence/AsyncDatabase.h` | 288 | All type definitions and class declarations (`QueryValue`, `QueryRow`, `QueryResult`, `PreparedStatementData`, `Transaction`, `IDatabaseConnection`, `SQLiteConnection`, `AsyncDatabasePool`) |
| `SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp` | 614 | Connection pool implementation, worker thread loop, query dispatch, SQLiteConnection file-based KV store |

## Error Handling

| Scenario | Behavior |
|----------|----------|
| `Open()` fails on any connection | Returns false; no workers are started |
| `Open()` called when already open | Returns false (no-op) |
| Unknown prepared statement ID | `Execute()` returns `QueryResult` with `success = false` and error message |
| Transaction query fails mid-batch | Remaining queries skipped, transaction rolled back, error propagated |
| `SyncQuery()` when pool is closed | Returns `QueryResult` with `success = false` |
| `Close()` with pending work items | Workers drain the queue before exiting |
| Worker thread exception | Not caught internally; undefined behavior. Ensure connection backends do not throw. |
| `GetInt()`/`GetDouble()`/`GetString()` type mismatch | Throws `std::bad_variant_access` |
| Column index out of range | `GetInt()`/`GetDouble()`/`GetString()` throw `std::out_of_range`; `IsNull()` returns true |

## Durable Commit (TERRAFRONT stores)

Every TERRAFRONT JSON store -- `TFDatabase` (accounts/characters), `TFOutfitStore`, `TFSocialSystem`, and the `WorldSave::WriteJson` world/progression files -- commits through one primitive, `Terrafront::SavePaths::WriteDurableReplace(destination, bytes, ec)` in `GameModules/SparkGameMMOFPS/Source/Persistence/TFSavePaths.h`. A crash or power loss leaves either the complete previous file or the complete new one, never an empty or truncated committed file.

| Step | POSIX | Windows |
|------|-------|---------|
| Stage | Unlink a stale `<store>.tmp` file (a directory there fails the write), create `<store>.tmp` with `O_CREAT\|O_EXCL\|O_NOFOLLOW\|O_CLOEXEC`, mode `0600` | `DeleteFileW` a stale staging file, `CreateFileW(CREATE_NEW)` |
| Write | Loop `write()` until every byte lands (retries `EINTR`) | Loop `WriteFile()` |
| Flush staging | `fsync()` then `close()`, both checked | `FlushFileBuffers()` then `CloseHandle()`, both checked |
| Swap | `rename()` over the destination | `MoveFileExW(MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH)` |
| Flush rename | `fsync()` the parent directory (`EINVAL`, "cannot sync a directory", is tolerated) | Covered by `MOVEFILE_WRITE_THROUGH` |

`false` means nothing was committed: any failure before the swap removes the staging file and leaves the destination untouched. `true` means the destination holds the new bytes and the caller must adopt them (TFDatabase keeps the new revision in memory). Once the swap has succeeded the commit is never reported as failed, because rolling the in-memory state back while the disk holds the new revision would make a purchase, account creation or transfer that the caller was told failed reappear on the next reload. If the POSIX parent-directory sync fails after the swap, the function still returns `true`, leaves the error in `ec` as a durability warning, and logs a `[TF] ... committed, but syncing its directory failed` warning; the next successful write restores durability. Callers must create the parent directory and hold the store's `ExclusiveFileLock` so there is one writer per destination.

Permissions: the owner-only (`0600`) committed file applies on POSIX only. On Windows the committed file inherits the parent directory's ACL, so the save root's ACL is the operator's responsibility.

Enforced by `Persistence_Durable_*` in `Tests/TestDATA120PersistenceReal.cpp` (byte-exact commit, stale staging discarded, failed staging leaves the committed file untouched, a stale symlink left at the staging path is unlinked rather than followed, committed files are owner-only on POSIX, and a directory-sync failure after the swap still reports the commit so TFDatabase keeps the new revision). The `O_EXCL|O_NOFOLLOW` create flags guard only an entry planted between that unlink and `open()`, which no test schedules; the source contract below pins that the flags stay present and by `Tests/Tools/test_async_database_durability.py` (CTest `AsyncDatabaseDurabilityContract`), which fails if the flush-before-swap and directory-sync ordering changes or any TERRAFRONT source writes or renames a file without the primitive.

## Secrets and Encryption at Rest (OD-22)

Owner decision OD-22 (`docs/readiness/OWNER-DECISIONS.md`, work item DATA-120) sets the stable-v1 rules for anything these stores write:

| Rule | How the tree meets it | Enforced by |
|------|-----------------------|-------------|
| Passwords are stored only as salted PBKDF2 hashes | TERRAFRONT `TFAccountSystem::Register` writes `pbkdf2-sha256$<iterations>$<saltHex>$<dkHex>` (150k iterations, 128-bit per-account salt) through `TFDatabase::CreateAccount`; the MMO account system uses `Spark::PasswordHash::Create` and keeps accounts in memory | `Persistence_Secrets_TFAccountStoreHoldsOnlySaltedPbkdf2Hashes` (row-column allowlist, no plaintext or hex-encoded password in the file, distinct salts for equal passwords, restart re-login from the hash alone) |
| Session tokens are never persisted | TERRAFRONT sessions are an in-memory client-id -> account-id map; MMO bearer tokens live only in `MMOAccountSystem`'s session table. Neither the TERRAFRONT JSON store nor the MMO key-value store (`MMOPersistenceSystem` over `AsyncDatabasePool`) has a session column | `Persistence_Secrets_TFLoginAndSessionBindingPersistOnlyLoginTime`, `Persistence_Secrets_MMOKeyValueStoreNeverReceivesPasswordOrSessionToken` (structural guard: `MMOAccountSystem` has no persistence path; the test confirms the written store holds no password, hash, or token and that a restarted account system rejects the old token) |
| No database secret in committed or shipped config | The only backend is the file-based `SQLiteConnection`; its "connection string" is a file path, and the TERRAFRONT store path comes from the `TF_SAVE_ROOT` environment variable (`TFSavePaths.h`). No persistence code reads a credential from a config file | `Persistence_Secrets_ShippedConfigCarriesNoDatabaseCredential` scans every config-like file under the trees the install rules ship config from (`SparkEngine/Resources/Config`, the runtime `Assets/` directories, and the `SparkServer/config` / `SparkGateway/config` operator examples installed to `share/SparkEngine/examples`); `Persistence_Secrets_CredentialScannerDetectsPlantedSecrets` proves the matcher catches planted keys and `user:password@` URLs |

**Operator responsibility -- encryption at rest.** stable-v1 does not encrypt database files itself (no SQLCipher or in-database encryption). Account stores hold usernames, salts, and PBKDF2 hashes; character stores hold gameplay state. Operators running a server must put the save directory (`TF_SAVE_ROOT`, default `<working-directory>/Saves`, and `mmo_data.db`, and their `.tmp`, `.bak`, and `.corrupt-*` siblings) on a volume protected by host full-disk encryption (BitLocker, LUKS/dm-crypt, FileVault, or the cloud provider's encrypted block storage), restrict the directory to the server account, and treat backups of it as sensitive.

**Adding a networked backend.** A future MySQL/PostgreSQL `IDatabaseConnection` must take its credential from the environment or the OS credential store at runtime, never from a shipped config file or source, and must not log the connection string (`AsyncDatabasePool` and `MMOPersistenceSystem` currently log the path they open, which is safe only because it is a file path).

## Implementing a New Backend

To add a real SQLite, MySQL, or PostgreSQL backend:

1. Create a new class implementing `IDatabaseConnection`
2. Implement all pure virtual methods (`Open`, `Close`, `PrepareStatement`, `Execute`, `ExecuteRaw`, `BeginTransaction`, `CommitTransaction`, `RollbackTransaction`)
3. Modify `AsyncDatabasePool::Open()` to construct your connection type instead of `SQLiteConnection`
4. The rest of the pool machinery (threading, queuing, callbacks) works unchanged

```cpp
class MySQLConnection : public IDatabaseConnection
{
public:
    bool Open(const std::string& connectionString) override { /* mysql_real_connect() */ }
    void Close() override { /* mysql_close() */ }
    // ... implement remaining methods ...
};
```

---

## See Also

- [Save System](Save-System.md) -- ECS-aware game state serialization
- [Networking](../subsystems/Networking.md) -- Multiplayer networking layer that may use AsyncDatabase for persistence
- [Entity Component System](../subsystems/Entity-Component-System.md) -- Components persisted by SaveSystem
- [Area Server Architecture](../subsystems/Area-Server-Architecture.md) -- Server architecture where AsyncDatabase fits naturally
