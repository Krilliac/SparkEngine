#!/usr/bin/env python3
"""Import a minimized campaign reproducer as a SEC-120 regression fixture.

``run_campaign.py`` keeps every crash/leak/timeout/OOM reproducer and its
minimized form, and lists the ones no corpus holds yet under
``unimported_findings``. This tool lands one of them the only way the policy
accepts:

* the minimized reproducer (never the raw one when a minimized form exists)
  is copied into the parser's reviewed corpus as ``regression-<slug>.<ext>``;
* the corpus manifest gains a ``regressions`` record, its ``content_digest``
  and ``last_verified`` are refreshed, and ``max_corpus_entries`` /
  ``max_corpus_bytes`` grow when the new seed needs them;
* the blocking smoke's ``-runs=N`` in the fuzz CMake file is raised to the new
  seed count so the replay covers the fixture.

The new record's ``finding`` and ``guard_test`` are written as the literal
placeholder ``TODO``. ``check_fuzz_policy.py`` rejects that placeholder, so an
import cannot land until a person records the finding and names the test that
fails without the fix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from corpus_manifest import (
    DEFAULT_CORPUS_MANIFEST,
    REGRESSION_FILE_PATTERN,
    REGRESSION_PLACEHOLDER,
    REPLAY_RUNS_FLAG,
    ResourceBudget,
    scan_corpus,
)
from parser_inventory import DEFAULT_INVENTORY, load_inventory
from policy_common import Deadline, PolicyError, canonical_root, load_json_document, read_confined_file

SLUG_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
MAX_SUMMARY_BYTES = 16 * 1024 * 1024
MAX_CMAKE_BYTES = 1024 * 1024
IMPORT_SCAN_SECONDS = 60


def _campaign_artifact(summary_path: Path, reproducer: Path, test_selector: str) -> dict[str, Any]:
    """The campaign artifact record ``reproducer`` belongs to, checked against its recorded hash."""
    if summary_path.stat().st_size > MAX_SUMMARY_BYTES:
        raise PolicyError(f"{summary_path} exceeds {MAX_SUMMARY_BYTES} bytes")
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if not isinstance(summary, dict) or summary.get("schema_version") != 1 or not isinstance(summary.get("targets"), list):
        raise PolicyError(f"{summary_path} is not a run_campaign.py summary")
    output = summary_path.parent.resolve()
    wanted = reproducer.resolve()
    for target in summary["targets"]:
        for artifact in target.get("artifacts", []) if isinstance(target, dict) else []:
            minimized = artifact.get("minimized")
            for role in ("minimized", "raw"):
                recorded = artifact.get(role)
                if not isinstance(recorded, str) or (output / recorded).resolve() != wanted:
                    continue
                if role == "raw" and minimized:
                    raise PolicyError(f"import the minimized reproducer {minimized}, not the raw one")
                if target.get("name") != test_selector:
                    raise PolicyError(
                        f"{recorded} was found by {target.get('name')!r}, not this parser's smoke {test_selector!r}"
                    )
                payload = reproducer.read_bytes()
                if hashlib.sha256(payload).hexdigest() != artifact.get(f"{role}_sha256"):
                    raise PolicyError(f"{recorded} no longer matches the sha256 the campaign recorded")
                return artifact
    raise PolicyError(f"{reproducer} is not a reproducer recorded in {summary_path}")


def _seed_extension(seeds: tuple[str, ...], requested: str | None) -> str:
    if requested is not None:
        return requested.lstrip(".")
    suffixes = {PurePosixPath(seed).suffix.lstrip(".") for seed in seeds}
    if len(suffixes) != 1 or not next(iter(suffixes)):
        raise PolicyError(f"the corpus mixes seed extensions {sorted(suffixes)}; pass --ext")
    return suffixes.pop()


def _raise_replay_runs(cmake_text: str, test_selector: str, old_runs: int, new_runs: int) -> str:
    """Rewrite ``-runs=<old>`` inside the add_test that registers ``test_selector``."""
    registration = re.search(r"add_test\s*\(\s*NAME\s+" + re.escape(test_selector) + r"\b[^)]*\)", cmake_text)
    if registration is None:
        raise PolicyError(f"no add_test registers {test_selector!r}")
    block = registration.group(0)
    runs = re.findall(re.escape(REPLAY_RUNS_FLAG) + r"(\d+)", block)
    if runs != [str(old_runs)]:
        raise PolicyError(f"add_test {test_selector!r} replays {runs}; expected exactly {REPLAY_RUNS_FLAG}{old_runs}")
    updated = block.replace(f"{REPLAY_RUNS_FLAG}{old_runs}", f"{REPLAY_RUNS_FLAG}{new_runs}")
    return cmake_text[: registration.start()] + updated + cmake_text[registration.end():]


def import_regression(
    root: Path,
    summary_path: Path,
    reproducer: Path,
    *,
    parser_id: str,
    slug: str,
    extension: str | None = None,
    manifest_path: str = DEFAULT_CORPUS_MANIFEST,
    inventory_path: str = DEFAULT_INVENTORY,
) -> Path:
    """Land one reproducer as a regression fixture; returns the new seed path."""
    root = canonical_root(root)
    if not SLUG_PATTERN.fullmatch(slug):
        raise PolicyError(f"--slug must match {SLUG_PATTERN.pattern}")
    parser = next((record for record in load_inventory(root, inventory_path).parsers if record.parser_id == parser_id),
                  None)
    if parser is None or parser.status != "fuzzed" or parser.target is None:
        raise PolicyError(f"{parser_id!r} is not a fuzzed parser in {inventory_path}")
    target = parser.target
    _campaign_artifact(summary_path, reproducer, target["test_selector"])

    document = load_json_document(root, manifest_path, "corpus_manifest")
    corpus = next((entry for entry in document["corpora"] if entry.get("id") == target["corpus_id"]), None)
    if corpus is None:
        raise PolicyError(f"{manifest_path} has no corpus {target['corpus_id']!r}")
    budget = ResourceBudget(**corpus["budget"])
    deadline = Deadline(IMPORT_SCAN_SECONDS, "regression import")
    seed_count, seed_bytes, _, seeds = scan_corpus(root, corpus["corpus_dir"], budget, "import", deadline)

    file_name = f"regression-{slug}.{_seed_extension(seeds, extension)}"
    if not REGRESSION_FILE_PATTERN.fullmatch(file_name):
        raise PolicyError(f"{file_name} is not a valid regression fixture name")
    destination = root / corpus["corpus_dir"] / file_name
    if destination.exists():
        raise PolicyError(f"{destination.relative_to(root).as_posix()} already exists")
    payload = reproducer.read_bytes()
    if not payload or len(payload) > budget.max_input_bytes:
        raise PolicyError(f"the reproducer is {len(payload)} bytes; seeds must be 1..{budget.max_input_bytes} bytes")

    cmake_file = target["cmake_file"]
    cmake_bytes = read_confined_file(root, cmake_file, "fuzz cmake file", max_bytes=MAX_CMAKE_BYTES)
    cmake_text = _raise_replay_runs(cmake_bytes.decode("utf-8"), target["test_selector"], seed_count, seed_count + 1)

    # Every check has passed: write the seed, then everything derived from it.
    with destination.open("xb") as stream:
        stream.write(payload)
    corpus["budget"]["max_corpus_entries"] = max(budget.max_corpus_entries, seed_count + 1)
    corpus["budget"]["max_corpus_bytes"] = max(budget.max_corpus_bytes, seed_bytes + len(payload))
    grown = ResourceBudget(**corpus["budget"])
    _, _, digest, _ = scan_corpus(root, corpus["corpus_dir"], grown, "import", deadline)
    corpus["content_digest"] = digest
    corpus["last_verified"] = datetime.now(timezone.utc).date().isoformat()
    corpus["regressions"].append(
        {
            "file": file_name,
            "finding": REGRESSION_PLACEHOLDER,
            "found_by": "campaign",
            "guard_test": REGRESSION_PLACEHOLDER,
        }
    )
    (root / cmake_file).write_bytes(cmake_text.encode("utf-8"))
    (root / manifest_path).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8", newline="\n")
    return destination


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("summary", type=Path, help="campaign-summary.json written by run_campaign.py")
    parser.add_argument("reproducer", type=Path, help="the minimized reproducer inside that campaign output")
    parser.add_argument("--parser", required=True, help="parser id from tools/fuzz-policy/parser-inventory.json")
    parser.add_argument("--slug", required=True, help="fixture name: regression-<slug>.<ext>")
    parser.add_argument("--ext", help="seed file extension (default: the corpus's single existing extension)")
    parser.add_argument("--source-root", type=Path, default=Path("."))
    args = parser.parse_args(argv)
    try:
        destination = import_regression(
            args.source_root, args.summary, args.reproducer, parser_id=args.parser, slug=args.slug, extension=args.ext
        )
    except (OSError, ValueError, KeyError, PolicyError) as exc:
        print(f"import_regression: FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        f"import_regression: added {destination}. Replace the {REGRESSION_PLACEHOLDER} finding and guard_test in "
        f"{DEFAULT_CORPUS_MANIFEST}; check_fuzz_policy.py fails until both are real."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
