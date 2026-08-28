"""Adversarial telemetry-spool validation regressions."""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ops"))
spool = importlib.import_module("validate_telemetry_spool")


def valid_event() -> dict[str, object]:
    now_ms = int(time.time() * 1000)
    return {
        "name": "level_complete",
        "timestamp": now_ms,
        "sessionId": f"session_{now_ms - 1000}",
        "properties": {},
    }


class TelemetrySpoolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.now = time.time()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def batch_name(self, offset_seconds: int = 0) -> str:
        return f"telemetry_{int((self.now + offset_seconds) * 1000)}.json"

    def write_batch(self, events: object, name: str | None = None) -> Path:
        path = self.root / (name or self.batch_name())
        path.write_text(json.dumps(events), encoding="utf-8")
        return path

    @staticmethod
    def checks(validator: spool.TelemetrySpoolValidator) -> set[str]:
        return {item.check for item in validator.errors}

    def validate(self) -> spool.TelemetrySpoolValidator:
        validator = spool.TelemetrySpoolValidator(now=self.now)
        validator.validate_spool_directory(self.root)
        return validator

    def test_valid_cpp_shaped_batch(self) -> None:
        self.write_batch([valid_event()])
        validator = self.validate()
        self.assertFalse(validator.errors)
        self.assertEqual(validator.stats["batch_files"], 1)

    def test_runtime_defaults_and_serialized_fields_remain_source_anchored(self) -> None:
        source = (ROOT / "SparkEngine" / "Source" / "Utils" / "Telemetry.h").read_text(encoding="utf-8")
        for declaration in (
            "uint32_t batchSize = 50",
            "float flushIntervalSeconds = 30.0f",
            "uint32_t maxQueueSize = 10000",
            r'\"name\": \"',
            r'\"timestamp\": ',
            r'\"sessionId\": \"',
            r'\"properties\": {',
        ):
            self.assertIn(declaration, source)

    def test_optional_sequence_uint64_is_accepted(self) -> None:
        event = valid_event()
        event["sequence"] = spool.UINT64_MAX
        self.write_batch([event])
        self.assertFalse(self.validate().errors)

    def test_unknown_entry_is_fatal_and_counted(self) -> None:
        unknown = self.root / "not-telemetry.bin"
        with unknown.open("wb") as output:
            output.seek(51 * 1024 * 1024)
            output.write(b"x")
        validator = self.validate()
        self.assertIn("unexpected-entry", self.checks(validator))
        self.assertIn("batch-open", self.checks(validator))
        self.assertGreater(validator.stats["total_bytes"], spool.MAX_SPOOL_BYTES)

    def test_malformed_batch_name_is_fatal(self) -> None:
        self.write_batch([valid_event()], "telemetry_bad.json")
        self.assertIn("unexpected-entry", self.checks(self.validate()))

    def test_twenty_digit_filename_timestamp_above_uint64_is_rejected(self) -> None:
        self.write_batch([valid_event()], "telemetry_99999999999999999999.json")
        self.assertIn("batch-name", self.checks(self.validate()))

    def test_subdirectory_is_fatal(self) -> None:
        (self.root / "nested").mkdir()
        self.assertIn("batch-open", self.checks(self.validate()))

    def test_hardlink_is_rejected_without_scanning_target(self) -> None:
        outside = self.root.parent / f"telemetry-outside-{self.root.name}.json"
        outside.write_text(json.dumps([{**valid_event(), "password": "real-value"}]), encoding="utf-8")
        try:
            os.link(outside, self.root / self.batch_name())
            validator = self.validate()
            self.assertIn("batch-open", self.checks(validator))
            self.assertNotIn("secret-in-event", self.checks(validator))
        finally:
            outside.unlink(missing_ok=True)

    @unittest.skipUnless(os.name == "nt", "Windows junction regression")
    def test_junction_root_is_rejected_by_cli(self) -> None:
        target = self.root / "target"
        target.mkdir()
        link = self.root / "junction"
        result = subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(link), str(target)],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode:
            self.skipTest(f"junctions unavailable: {result.stderr}")
        try:
            self.assertEqual(spool.main([str(link)]), 1)
        finally:
            os.rmdir(link)

    def test_old_filename_timestamp_is_rejected(self) -> None:
        self.write_batch([valid_event()], self.batch_name(-spool.MAX_RETENTION_SECONDS - 10))
        self.assertIn("retention", self.checks(self.validate()))

    def test_old_mtime_is_rejected_independently(self) -> None:
        path = self.write_batch([valid_event()])
        old = self.now - spool.MAX_RETENTION_SECONDS - 10
        os.utime(path, (old, old))
        self.assertIn("retention", self.checks(self.validate()))

    def test_future_filename_is_rejected(self) -> None:
        self.write_batch([valid_event()], self.batch_name(spool.MAX_CLOCK_SKEW_SECONDS + 10))
        self.assertIn("future-time", self.checks(self.validate()))

    def test_duplicate_json_keys_are_rejected(self) -> None:
        path = self.root / self.batch_name()
        path.write_text(
            '[{"name":"a","name":"b","timestamp":1,"sessionId":"s","properties":{}}]',
            encoding="utf-8",
        )
        self.assertIn("batch-json", self.checks(self.validate()))

    def test_nan_and_overflow_float_are_rejected(self) -> None:
        for token in ("NaN", "Infinity", "1e999"):
            with self.subTest(token=token):
                path = self.root / self.batch_name()
                path.write_text(
                    f'[{{"name":"a","timestamp":{token},"sessionId":"s","properties":{{}}}}]',
                    encoding="utf-8",
                )
                self.assertIn("batch-json", self.checks(self.validate()))

    def test_missing_session_id_is_rejected(self) -> None:
        event = valid_event()
        del event["sessionId"]
        self.write_batch([event])
        self.assertIn("event-fields", self.checks(self.validate()))

    def test_empty_session_id_is_rejected(self) -> None:
        event = valid_event()
        event["sessionId"] = ""
        self.write_batch([event])
        self.assertIn("session-id", self.checks(self.validate()))

    def test_non_runtime_session_id_shape_is_rejected(self) -> None:
        event = valid_event()
        event["sessionId"] = "session-not-an-epoch"
        self.write_batch([event])
        self.assertIn("session-id", self.checks(self.validate()))

    def test_mixed_sessions_in_one_batch_are_rejected(self) -> None:
        first = valid_event()
        second = valid_event()
        second["sessionId"] = "session_2"
        self.write_batch([first, second])
        self.assertIn("batch-session", self.checks(self.validate()))

    def test_bool_float_negative_and_overflow_timestamps_are_rejected(self) -> None:
        for value in (True, 1.5, -1, spool.UINT64_MAX + 1):
            with self.subTest(value=value):
                event = valid_event()
                event["timestamp"] = value
                self.write_batch([event])
                self.assertIn("event-timestamp", self.checks(self.validate()))

    def test_sequence_type_and_range_are_strict(self) -> None:
        for value in (False, 0, 1.5, -1, spool.UINT64_MAX + 1):
            with self.subTest(value=value):
                event = valid_event()
                event["sequence"] = value
                self.write_batch([event])
                self.assertIn("event-sequence", self.checks(self.validate()))

    def test_mixed_or_nonincreasing_sequences_are_rejected(self) -> None:
        first = valid_event()
        second = valid_event()
        first["sequence"] = 2
        self.write_batch([first, second])
        self.assertIn("batch-sequence", self.checks(self.validate()))

        second["sequence"] = 1
        self.write_batch([first, second])
        self.assertIn("batch-sequence", self.checks(self.validate()))

    def test_properties_are_required(self) -> None:
        event = valid_event()
        del event["properties"]
        self.write_batch([event])
        validator = self.validate()
        self.assertIn("event-fields", self.checks(validator))
        self.assertIn("properties", self.checks(validator))

    def test_property_values_must_be_strings(self) -> None:
        for value in (1, True, None, [], {}):
            with self.subTest(value=value):
                event = valid_event()
                event["properties"] = {"key": value}
                self.write_batch([event])
                self.assertIn("property-value", self.checks(self.validate()))

    def test_unknown_event_fields_are_rejected(self) -> None:
        event = valid_event()
        event["consent"] = True
        self.write_batch([event])
        self.assertIn("event-fields", self.checks(self.validate()))

    def test_control_characters_are_rejected(self) -> None:
        event = valid_event()
        event["name"] = "bad\x00name"
        self.write_batch([event])
        self.assertIn("event-name", self.checks(self.validate()))

    def test_property_limits_apply_to_every_entry(self) -> None:
        event = valid_event()
        event["properties"] = {f"key{i}": "v" for i in range(spool.MAX_EVENT_PROPERTIES + 1)}
        self.write_batch([event])
        self.assertIn("properties", self.checks(self.validate()))

    def test_structured_and_token_secrets_are_rejected(self) -> None:
        secrets = {
            "password": "real-value",
            "github": "ghs_" + "A" * 40,
            "aws": "ASIA" + "A" * 16,
            "openai": "sk-proj-" + "A" * 30,
        }
        event = valid_event()
        event["properties"] = secrets
        self.write_batch([event])
        self.assertIn("secret-in-event", self.checks(self.validate()))

    def test_batch_event_count_is_bounded(self) -> None:
        with mock.patch.object(spool, "MAX_EVENTS_PER_BATCH", 2):
            self.write_batch([valid_event(), valid_event(), valid_event()])
            self.assertIn("batch-json", self.checks(self.validate()))

    def test_directory_file_count_is_bounded(self) -> None:
        (self.root / "a").write_text("a", encoding="utf-8")
        (self.root / "b").write_text("b", encoding="utf-8")
        with mock.patch.object(spool, "MAX_SPOOL_FILES", 1):
            self.assertIn("spool-root", self.checks(self.validate()))

    def test_missing_cli_root_is_nonzero(self) -> None:
        self.assertEqual(spool.main([str(self.root / "missing")]), 1)


if __name__ == "__main__":
    unittest.main()
