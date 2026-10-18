#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the SparkDaemon orchestration journal recovery path.

Each seed is the harness framing FuzzOrchestrationJournalProduction.cpp decodes:
one flag byte (bit 0 writes the snapshot file, bit 1 the .wal file), a
little-endian u32 snapshot length, the snapshot bytes, then the WAL bytes. The
snapshot and WAL layouts are the ones WriteOrchestrationJournal and
AppendOrchestrationIntent/Commit write.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "daemon-orchestration-journal"

MAGIC = b"SPORCH2"
WAL_MAGIC = 0x4C415753
SCHEMA_VERSION = 1
INTENT = 1
COMMIT = 2
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def u64(value: int) -> bytes:
    return struct.pack("<Q", value)


def i64(value: int) -> bytes:
    return struct.pack("<q", value)


def string(value: bytes) -> bytes:
    return u32(len(value)) + value


def key(client: bytes, sequence: int) -> bytes:
    return string(client) + u64(sequence)


def definition(process_id: bytes) -> bytes:
    """EncodeProcessDefinition with the journal key the writer uses."""
    payload = u16(SCHEMA_VERSION) + key(b"journal", 1) + string(process_id)
    payload += string(b"/opt/spark/bin/SparkServer") + string(b"/opt/spark")
    payload += u32(1) + string(b"--headless")
    payload += bytes([1]) + u32(5000)  # RestartPolicy::OnFailure, graceful stop ms
    return payload


def status(process_id: bytes, drain_deadline: int = 0) -> bytes:
    """EncodeProcessStatuses holding one status."""
    payload = u16(SCHEMA_VERSION) + u32(1) + string(process_id)
    payload += bytes([2]) + i64(4242) + u32(1) + struct.pack("<i", 0)  # Running, pid, restarts, exit code
    payload += bytes([1]) + u64(0x1234) + u32(0) + i64(drain_deadline)  # Healthy, start token, crash loops
    return payload


def process(process_id: bytes, crashes: list[int], drain_deadline: int = 0) -> bytes:
    record = string(definition(process_id)) + string(status(process_id, drain_deadline))
    record += bytes([1]) + u32(len(crashes))
    return record + b"".join(i64(timestamp) for timestamp in crashes)


def mutation(client: bytes, sequence: int) -> bytes:
    return string(client) + u64(sequence) + u16(0x0006) + string(b"\x01\x00ok")


def snapshot(processes: list[bytes], mutations: list[bytes], process_count: int | None = None) -> bytes:
    payload = string(MAGIC) + u16(SCHEMA_VERSION)
    payload += u32(len(processes) if process_count is None else process_count) + b"".join(processes)
    return payload + u32(len(mutations)) + b"".join(mutations)


def fnv1a64(payload: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in payload:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def wal_record(payload: bytes) -> bytes:
    return u32(WAL_MAGIC) + u32(len(payload)) + u64(fnv1a64(payload)) + string(payload)


def intent(client: bytes, sequence: int, process_id: bytes) -> bytes:
    return wal_record(bytes([INTENT]) + key(client, sequence) + u16(0x0005) + string(process_id) + i64(0) + u64(0))


def commit(client: bytes, sequence: int) -> bytes:
    return wal_record(bytes([COMMIT]) + key(client, sequence))


def frame(snapshot_bytes: bytes | None, wal_bytes: bytes | None) -> bytes:
    flags = (1 if snapshot_bytes is not None else 0) | (2 if wal_bytes is not None else 0)
    snap = snapshot_bytes or b""
    return bytes([flags]) + u32(len(snap)) + snap + (wal_bytes or b"")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    now = 1_790_000_000_000
    base = snapshot([process(b"server-a", [now - 5_000, now - 1_000])], [mutation(b"cli-0001", 7)])
    torn = intent(b"cli-0002", 3, b"server-a")
    seeds = {
        "valid-one-process.bin": frame(base, None),
        "valid-no-journal.bin": frame(None, None),
        "wal-open-intent.bin": frame(base, intent(b"cli-0002", 3, b"server-a")),
        "wal-intent-then-commit.bin": frame(base, intent(b"cli-0002", 3, b"server-a") + commit(b"cli-0002", 3)),
        "wal-torn-tail.bin": frame(base, intent(b"cli-0003", 1, b"server-a") + torn[: len(torn) - 5]),
        "count-overclaim.bin": frame(snapshot([], [], process_count=65), None),
        # Accepted before the loader bounded persisted Unix times: OrchestrationService
        # converts each to a system_clock time point, and INT64_MIN ms overflows it.
        "regression-unrepresentable-crash-timestamp.bin": frame(
            snapshot([process(b"server-a", [INT64_MIN])], []), None
        ),
        "regression-unrepresentable-drain-deadline.bin": frame(
            snapshot([process(b"server-a", [], drain_deadline=INT64_MAX)], []), None
        ),
        # Accepted before: the writer serializes keyed maps, so two records for one
        # process id or one client instance can only come from a damaged file.
        "regression-duplicate-process-id.bin": frame(
            snapshot([process(b"server-a", []), process(b"server-a", [])], []), None
        ),
        "regression-duplicate-client-instance.bin": frame(
            snapshot([], [mutation(b"cli-0001", 7), mutation(b"cli-0001", 8)]), None
        ),
        # Accepted before: ReadMutationKey refuses an empty client instance, the
        # snapshot's mutation reader did not.
        "regression-empty-client-instance.bin": frame(snapshot([], [mutation(b"", 7)]), None),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
