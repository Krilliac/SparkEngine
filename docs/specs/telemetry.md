# Telemetry output and offline validation

This document describes the current telemetry runtime in `SparkEngine/Source/Utils/Telemetry.h` (with its durable spool in `SparkEngine/Source/Utils/TelemetrySpool*.cpp`) and the stricter offline validator in `tools/ops/validate_telemetry_spool.py`. The runtime bounds below are enforced by C++ source constants and pinned by local `TelemetrySpool` tests. This document does not claim remote delivery: the only shipped backend is `LocalFileTelemetryBackend`, so "endpoint outage" means a backend that reports `RetryableFailure`, exercised through test backends, not a hosted endpoint. `TelemetryConfig::httpEndpoint` is unused.

## Runtime output

`TelemetrySystem` records events only when initialized, enabled, and consent is observed as true. `LocalFileTelemetryBackend` writes a JSON array to `telemetry_<epoch-ms>_<first-sequence>_<last-sequence>.json`. Each serialized event has exactly the runtime fields below:

```json
{
  "name": "level_complete",
  "timestamp": 1712188800000,
  "sessionId": "session_1712188700000",
  "properties": {"level": "3"}
}
```

`name`, `sessionId`, property keys, and property values are strings. `timestamp` is an unsigned 64-bit epoch-millisecond value. The in-memory event also has an unsigned 64-bit `sequence`, used to sort a flush; the current local serializer does not write it. The offline validator permits `sequence` only as an optional uint64 compatibility field and rejects all other unknown event fields.

## Runtime bounds, spool, and retry

All values are `TelemetryConfig` defaults unless noted; a caller may lower them.

| Bound | Default | Enforced where | Pinned by |
|-------|---------|----------------|-----------|
| In-memory queue events (`maxQueueSize`) | 10,000 | `EnqueueEventThreadSafe` drops and counts `droppedEvents` | `Telemetry_SpoolRecovery_CapDropAccounting` |
| In-memory queue bytes (`maxQueueBytes`) | 4 MiB (conservative per-event estimate) | same | `Telemetry_SpoolRecovery_CapDropAccounting` |
| Durable spool events (`maxSpoolEvents`) | 10,000; `Configure` rejects 0 or more than 100,000 (`kAbsoluteMaxEvents`) | `TelemetrySpool::Constrain` keeps the oldest events | `Telemetry_SpoolRecovery_CapDropAccounting` |
| Durable spool bytes (`maxSpoolBytes`) | 4 MiB including the 16-byte header | same | `Telemetry_SpoolRecovery_CapDropAccounting` |
| Spool strings / properties | 1 MiB per string, 256 properties per event | `TelemetrySpoolFormat.cpp` reader and writer | source constants; malformed/oversized artifacts: `Telemetry_SpoolRecovery_HostileArtifactRejection` |
| Batch size (`batchSize`) | 50 events | `FlushEvents` | `Telemetry_SpoolRecovery_RejectedIsTerminal` (batch of 2) |
| Normal flush cadence (`flushIntervalSeconds`) | 30 s | `Update(dt)` | source default; retry-versus-flush precedence: `Telemetry_SpoolRecovery_UpdateRetriesAtBoundary` |
| Retry cadence (`retryIntervalSeconds`) | 5 s after a retryable failure or pending spool work | `Update(dt)` | `Telemetry_SpoolRecovery_UpdateRetriesAtBoundary` |

The spool is enabled only when `spoolDirectory` is set. It owns exactly two fixed names in that directory, `spark-telemetry.spool` and the atomic staging file `spark-telemetry.spool.tmp`, and never removes the directory or any other entry. A flush first commits the bounded pending set to the spool, then sends batches to the backend:

- `Delivered` and `Rejected` are terminal. The durable cursor advances only after a terminal result. A rejected batch is counted in `rejectedEvents` and is never retried.
- `RetryableFailure` (or a backend exception) keeps the remaining events queued and spooled and schedules a retry after `retryIntervalSeconds`. After a restart, `Initialize` restores the committed spool before new events are delivered.
- Delivery is at-least-once. If a cursor update fails after a terminal result, replay can repeat a delivered event, so the per-event `sequence` is the deduplication key.
- Capacity loss is published in `droppedEvents` only after the bounded durable snapshot has committed. Events that did not fit remain accounted, not silently lost. `TelemetryDeliveryStats` also reports queued, carryover, spooled, delivered, rejected, retryable-failed, spool I/O failure, and rejected spool-operation counts.
- A spool directory or artifact that is a symlink, hard link, or reparse point, or an artifact that is malformed or oversized, is rejected without being read, followed, or deleted (`Telemetry_SpoolRecovery_SymlinkRejection`, `Telemetry_SpoolRecovery_HostileArtifactRejection`).

Without a spool directory, a retryable failure keeps events in memory only, and `Shutdown` counts them as dropped.

## Consent semantics

Consent is reversible through `SetConsent(false)`. Revocation stops recording, clears the in-memory queue and shutdown carryover (counted in `droppedEvents`), and purges only the fixed spool artifacts. The purge is retried on the retry cadence if it cannot complete immediately. `Telemetry_SpoolRecovery_ConsentRevocation` proves that no backend call follows revocation, both spool files are removed, and a caller-owned file in the same directory survives.

Revocation increments a recording generation. `RecordEvent` captures the generation before it builds an event, and the enqueue path rechecks consent and that generation while holding the queue lock. A producer that passed the first consent check just before another thread revoked consent therefore cannot enqueue afterwards. `Initialize` records the game thread; `SetConsent`, `Shutdown`, `FlushEvents`, and `Update` assert they run on it.

## Offline spool policy

Run:

```text
python tools/ops/validate_telemetry_spool.py <spool-root>
python tools/ops/redact_secrets.py <spool-root>
```

The validator pins the supplied non-reparse root; accepts only immediate `telemetry_<epoch-ms>_<first-sequence>_<last-sequence>.json` (and the legacy `telemetry_<epoch-ms>.json`) regular single-link files; and rejects symlinks, junctions/reparse points, hard links, subdirectories, and unmatched filenames. Every entry, including an unmatched or oversized one, participates in count and aggregate-size accounting.

JSON is strict UTF-8 with duplicate keys, non-finite numbers, excessive depth, excessive collections, and oversized strings rejected. Events require `name`, `timestamp`, `sessionId`, and `properties`; timestamps and optional sequences must be true uint64 integers (not booleans or floats); properties must be a bounded string-to-string object. Filename time and filesystem modification time are checked independently for retention and future skew. The shared secret policy covers GitHub, AWS, OpenAI, Anthropic, bearer/URL/PEM, and structured credential fields without printing secret previews.

The validator's 50 MiB aggregate, 1 MiB file, 1,000-entry, seven-day retention, event/property, archive, and wall-time limits are **Python-only offline policy** for exported `telemetry_*.json` files. The C++ runtime does not enforce those export-file retention limits. Passing this tool is not evidence of outage recovery, durable retry, drop accounting, or consent correctness; the C++ tests above are the evidence for those.

## OPS-100 blockers

- No network backend or controlled relay exists, so no hosted outage/recovery evidence exists. The bounds above are local, test-proven behavior only.
- Exported `telemetry_*.json` files have no runtime retention or deletion policy.
- `telemetry-integration` has no successful exact-SHA hosted record on the Working branch yet.
- The privacy/retention policy and operations runbook still need review.
