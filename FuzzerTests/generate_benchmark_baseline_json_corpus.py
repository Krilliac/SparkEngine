#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the benchmark baseline reader.

Each seed is a baseline file as BenchmarkFramework::LoadBaseline reads it: the line-oriented JSON
that BenchmarkFramework::SaveBaseline writes ("baselines" array of {"scenario", "metrics": {...},
"tolerance"} objects, one key per line), or a damaged variant. The reader is a line scraper, not a
JSON parser, so several seeds are deliberately not valid JSON. The fuzz adapter
(FuzzBenchmarkBaselineJsonProduction.cpp) writes each seed to a file and loads it through
LoadBaseline, then compares fixed results against what was loaded.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "benchmark-baseline-json"


def scenario(name: str, metrics: list[tuple[str, str]], tolerance: str | None = "5.0") -> str:
    """One baseline object laid out exactly as SaveBaseline writes it."""
    lines = ["    {", f'      "scenario": "{name}",', '      "metrics": {']
    for index, (metric, value) in enumerate(metrics):
        comma = "," if index + 1 < len(metrics) else ""
        lines.append(f'        "{metric}": {value}{comma}')
    if tolerance is None:
        lines.append("      }")
    else:
        lines.append("      },")
        lines.append(f'      "tolerance": {tolerance}')
    lines.append("    }")
    return "\n".join(lines)


def document(*objects: str) -> str:
    return '{\n  "baselines": [\n' + ",\n".join(objects) + "\n  ]\n}\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    alpha = scenario("alpha", [("FrameTime", "12.5"), ("Throughput", "480")])
    beta = scenario("beta", [("SortTime", "3.25")], "10")
    valid = document(alpha, beta)

    seeds = {
        "valid-two-scenarios.json": valid.encode(),
        "valid-crlf-line-endings.json": valid.replace("\n", "\r\n").encode(),
        "valid-no-tolerance.json": document(scenario("alpha", [("FrameTime", "12.5")], None)).encode(),
        "valid-empty-metrics.json": document(scenario("alpha", [])).encode(),
        "escaped-scenario-name.json": document(scenario('quote\\"inside\\\\path', [("a", "1")])).encode(),
        "metric-non-finite.json": document(
            scenario("alpha", [("NotANumber", "nan"), ("Positive", "inf"), ("Negative", "-inf"), ("Hex", "0x10")])
        ).encode(),
        "metric-out-of-range.json": document(
            scenario("alpha", [("Huge", "1e999"), ("Tiny", "1e-999"), ("Long", "9" * 400)], "1e999")
        ).encode(),
        "metric-garbage-values.json": document(
            scenario("alpha", [("Word", "abc"), ("Blank", ""), ("Trailing", "1.5.5"), ("Zero", "0")], "abc")
        ).encode(),
        "tolerance-negative.json": document(scenario("alpha", [("FrameTime", "12.5")], "-1e39")).encode(),
        "metric-name-unterminated.json": b'{\n  "baselines": [\n    {\n      "scenario": "alpha",\n'
        b'      "metrics": {\n        "unterminated: 1\n      },\n      "tolerance": 5.0\n    }\n  ]\n}\n',
        "scenario-without-value.json": b'{\n  "baselines": [\n    {\n      "scenario"\n      "metrics": {\n'
        b'        "a": 1\n      },\n    }\n  ]\n}\n',
        "scenario-unterminated-value.json": b'{\n    {\n      "scenario": "never closed\n      "metrics": {\n'
        b'        "a": 1\n      }\n    }\n',
        "metrics-block-never-closed.json": b'{\n    {\n      "scenario": "alpha",\n      "metrics": {\n'
        b'        "a": 1,\n        "b": 2\n    {\n      "scenario": "beta",\n',
        "minified-single-line.json": b'{"baselines":[{"scenario":"alpha","metrics":{"a":1},"tolerance":5.0}]}',
        "truncated.json": valid.encode()[: len(valid) // 2],
        "binary-nul-bytes.json": valid.encode().replace(b"alpha", b"al\x00ha").replace(b"12.5", b"1\x002.5"),
        "long-scenario-name.json": document(scenario("n" * 6000, [("a", "1")])).encode(),
        "many-metrics.json": document(scenario("alpha", [(f"metric{i}", str(i * 7 % 101)) for i in range(200)])).encode(),
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(seeds.items()):
        (args.output / name).write_bytes(data)
        print(f"{name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
