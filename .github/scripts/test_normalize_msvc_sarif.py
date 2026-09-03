#!/usr/bin/env python3
"""Regression tests for the MSVC SARIF translation-unit coalescer."""

from __future__ import annotations

from copy import deepcopy
import hashlib
import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).with_name("normalize-msvc-sarif.py")
SPEC = importlib.util.spec_from_file_location("normalize_msvc_sarif", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
NORMALIZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NORMALIZER)


def artifact(name: str) -> dict[str, object]:
    return {
        "location": {"uri": f"file:///d:/{name}.cpp"},
        "roles": ["analysisTarget", "resultFile"],
        "hashes": {"sha-256": hashlib.sha256(name.encode()).hexdigest()},
    }


def run(unit: int, artifacts: list[dict[str, object]]) -> dict[str, object]:
    return {
        "tool": {
            "driver": {
                "name": "PREfast",
                "version": "19.44",
                "rules": [{"id": "C6001"}, {"id": "C6011"}],
            }
        },
        "artifacts": deepcopy(artifacts),
        "invocations": [
            {
                "executionSuccessful": True,
                "executableLocation": {"index": 0},
                "workingDirectory": {"index": 0},
                "responseFiles": [{"index": len(artifacts) - 1}],
            }
        ],
        "results": [
            {
                "ruleId": "C6001" if unit % 2 == 0 else "C6011",
                "ruleIndex": unit % 2,
                "message": {"text": f"unit:{unit}"},
                "analysisTarget": {"index": 0},
                "locations": [
                    {"physicalLocation": {"artifactLocation": {"index": 0}}}
                ],
                "codeFlows": [
                    {
                        "threadFlows": [
                            {
                                "locations": [
                                    {
                                        "location": {
                                            "physicalLocation": {
                                                "artifactLocation": {
                                                    "index": len(artifacts) - 1
                                                }
                                            }
                                        }
                                    }
                                ]
                            }
                        ]
                    }
                ],
                "fixes": [
                    {
                        "artifactChanges": [
                            {
                                "artifactLocation": {"index": len(artifacts) - 1},
                                "replacements": [
                                    {
                                        "deletedRegion": {
                                            "startLine": 1,
                                            "startColumn": 1,
                                            "endLine": 1,
                                            "endColumn": 2,
                                        },
                                        "insertedContent": {"text": ""},
                                    }
                                ],
                            }
                        ]
                    }
                ],
                "provenance": {"invocationIndex": 0},
            }
        ],
    }


class NormalizeMsvcSarifTests(unittest.TestCase):
    def test_vendored_findings_are_dropped_before_consolidation(self) -> None:
        engine = artifact("a/SparkEngine/SparkEngine/SparkEngine/Source/Core/Engine")
        imgui = artifact("a/SparkEngine/SparkEngine/ThirdParty/UI/imgui/imgui")
        kept_by_index = run(0, [engine, imgui])
        dropped_by_index = run(1, [imgui, engine])
        dropped_by_uri = run(2, [engine])
        dropped_by_uri["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"] = {
            "uri": "file:///d:/a/SparkEngine/SparkEngine/build/_deps/fmt-src/src/format.cc"
        }
        dropped_by_backslash_uri = run(3, [engine])
        dropped_by_backslash_uri["results"][0]["locations"][0]["physicalLocation"][
            "artifactLocation"
        ] = {"uri": "d:\\a\\SparkEngine\\SparkEngine\\build\\ThirdParty\\AngelScriptPatched\\source\\as_map.h"}
        header_in_engine_unit = run(4, [engine, imgui])
        header_in_engine_unit["results"][0]["locations"][0]["physicalLocation"]["artifactLocation"] = {
            "index": 1
        }
        payload = {
            "version": "2.1.0",
            "runs": [
                kept_by_index,
                dropped_by_index,
                dropped_by_uri,
                dropped_by_backslash_uri,
                header_in_engine_unit,
            ],
        }

        self.assertEqual(NORMALIZER.drop_vendored(payload), 4)
        remaining = [result for run_ in payload["runs"] for result in run_["results"]]
        self.assertEqual([result["message"]["text"] for result in remaining], ["unit:0"])

        NORMALIZER.normalize(payload)
        merged = [result for run_ in payload["runs"] for result in run_["results"]]
        self.assertEqual([result["message"]["text"] for result in merged], ["unit:0"])
        self.assertEqual(NORMALIZER.drop_vendored(payload), 0)

    def test_shared_artifacts_are_deduplicated_and_all_references_are_remapped(self) -> None:
        shared = artifact("shared")
        payload = {
            "version": "2.1.0",
            "runs": [
                run(0, [shared, artifact("first")]),
                run(1, [shared, artifact("second")]),
            ],
        }

        self.assertEqual(NORMALIZER.normalize(payload), ["msvc/prefast/"])
        merged = payload["runs"][0]
        self.assertEqual(len(merged["artifacts"]), 3)
        self.assertEqual(len(merged["results"]), 2)
        self.assertEqual(len(merged["invocations"]), 2)

        first, second = merged["results"]
        self.assertEqual(first["analysisTarget"]["index"], 0)
        self.assertEqual(second["analysisTarget"]["index"], 0)
        self.assertEqual(first["fixes"][0]["artifactChanges"][0]["artifactLocation"]["index"], 1)
        self.assertEqual(second["fixes"][0]["artifactChanges"][0]["artifactLocation"]["index"], 2)
        first_flow_location = first["codeFlows"][0]["threadFlows"][0]["locations"][0]
        second_flow_location = second["codeFlows"][0]["threadFlows"][0]["locations"][0]
        self.assertEqual(
            first_flow_location["location"]["physicalLocation"]["artifactLocation"]["index"],
            1,
        )
        self.assertEqual(
            second_flow_location["location"]["physicalLocation"]["artifactLocation"]["index"],
            2,
        )
        self.assertEqual(first["provenance"]["invocationIndex"], 0)
        self.assertEqual(second["provenance"]["invocationIndex"], 1)
        self.assertEqual(merged["invocations"][0]["responseFiles"][0]["index"], 1)
        self.assertEqual(merged["invocations"][1]["responseFiles"][0]["index"], 2)

    def test_existing_generated_category_can_be_repaired_idempotently(self) -> None:
        shared = artifact("shared")
        payload = {
            "version": "2.1.0",
            "runs": [
                {
                    **run(0, [shared, shared]),
                    "automationDetails": {"id": "msvc/prefast/"},
                }
            ],
        }

        NORMALIZER.normalize(payload)
        self.assertEqual(len(payload["runs"][0]["artifacts"]), 1)
        self.assertEqual(payload["runs"][0]["automationDetails"]["id"], "msvc/prefast/")

    def test_more_than_twenty_tool_groups_fail_closed(self) -> None:
        payload = {
            "version": "2.1.0",
            "runs": [
                {
                    **run(index, [artifact(str(index))]),
                    "tool": {"driver": {"name": "PREfast", "version": str(index)}},
                }
                for index in range(21)
            ],
        }

        with self.assertRaisesRegex(ValueError, "GitHub accepts 20"):
            NORMALIZER.normalize(payload)

    def test_unsafe_indexed_shapes_fail_closed(self) -> None:
        payload = {"version": "2.1.0", "runs": [run(0, [artifact("one")])]}
        payload["runs"][0]["tool"]["driver"]["locations"] = [{"index": 0}]
        with self.assertRaisesRegex(ValueError, "tool-component locations"):
            NORMALIZER.normalize(payload)


if __name__ == "__main__":
    unittest.main()
