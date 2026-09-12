# Telemetry output and offline validation

This document describes the current local telemetry implementation in `SparkEngine/Source/Utils/Telemetry.h` and the stricter offline validator in `tools/ops/validate_telemetry_spool.py`. It does not claim a durable runtime spool or remote delivery.

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

Runtime defaults are 10,000 queued events, 50 events per batch, and a 30-second flush interval. A full queue silently drops new events. Backend failure is not durably spooled, retried, or fully accounted today.

## Consent semantics and known race

Consent is reversible through `SetConsent(false)`, which clears the queue under the queue mutex. Future `RecordEvent` calls normally fail the `CanRecord()` gate.

This is not yet a complete concurrent no-event-after-revocation guarantee. A producer can pass `CanRecord()` immediately before another thread revokes consent, then enqueue after the queue was cleared because enqueue does not recheck consent while holding the queue lock. Flush/lifecycle coordination also needs a reviewed contract. This producer race is a blocking OPS-100 runtime issue; the offline validator cannot prove consent from a file and deliberately makes no consent claim.

## Offline spool policy

Run:

```text
python tools/ops/validate_telemetry_spool.py <spool-root>
python tools/ops/redact_secrets.py <spool-root>
```

The validator pins the supplied non-reparse root; accepts only immediate `telemetry_<epoch-ms>_<first-sequence>_<last-sequence>.json` (and the legacy `telemetry_<epoch-ms>.json`) regular single-link files; and rejects symlinks, junctions/reparse points, hard links, subdirectories, and unmatched filenames. Every entry, including an unmatched or oversized one, participates in count and aggregate-size accounting.

JSON is strict UTF-8 with duplicate keys, non-finite numbers, excessive depth, excessive collections, and oversized strings rejected. Events require `name`, `timestamp`, `sessionId`, and `properties`; timestamps and optional sequences must be true uint64 integers (not booleans or floats); properties must be a bounded string-to-string object. Filename time and filesystem modification time are checked independently for retention and future skew. The shared secret policy covers GitHub, AWS, OpenAI, Anthropic, bearer/URL/PEM, and structured credential fields without printing secret previews.

The validator's 50 MiB aggregate, 1 MiB file, 1,000-entry, seven-day retention, event/property, archive, and wall-time limits are **Python-only offline policy**. The present C++ telemetry runtime does not enforce those spool/retention/file limits, and passing this tool is not evidence of outage recovery, durable retry, drop accounting, or consent correctness.

## OPS-100 blockers

- Close the producer/revocation race and define lifecycle synchronization.
- Implement bounded durable spool, retry, retention, and explicit drop/failure accounting in C++.
- Add outage/recovery and consent integration tests plus required CI jobs.
- Review the privacy/retention policy and operations runbook.
