#!/usr/bin/env python3
"""Adversarial tests for the CI-110 whole-test and per-assertion waiver policy validator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPO_ROOT / "Tools" / "validate_test_warnings.py"
AS_OF = "2026-09-13"


def registry(*patterns: str) -> str:
    entries = ",\n".join(f'    {{"{pattern}", "known reason"}}' for pattern in patterns)
    return (
        "inline constexpr TestWarningPattern g_testWarningPatterns[] = {\n"
        f"{entries}\n"
        "};\n"
    )


def metadata(*entries: dict[str, str]) -> str:
    return json.dumps({"schemaVersion": 1, "waivers": list(entries)}) + "\n"


def metadata_v2(entries: list[dict[str, str]], assertion_entries: list[dict[str, object]]) -> str:
    return json.dumps({"schemaVersion": 2, "waivers": entries, "assertionWaivers": assertion_entries}) + "\n"


def assertion_waiver(
    file: str = "Tests/TestLoad.cpp",
    test: str = "Load_Spikes",
    sites: int = 1,
    owner: str = "engine-core",
    expires: str = "2026-09-14",
) -> dict[str, object]:
    return {"file": file, "test": test, "sites": sites, "owner": owner, "expires": expires}


LOAD_TEST_SOURCE = """#include "TestFramework.h"

TEST(Load_Spikes)
{
    int spikes = 3;
    EXPECT_TRUE(spikes >= 0);
    EXPECT_WARN_ONLY(spikes <= 30, "host scheduling pressure");
}
"""


def waiver(pattern: str, owner: str = "engine-qa", expires: str = "2026-09-14") -> dict[str, str]:
    return {"pattern": pattern, "owner": owner, "expires": expires}


class ValidatorHarness(unittest.TestCase):
    def run_validator(
        self, registry_text: str, metadata_text: str, sources: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            registry_path = root / "TestWarnings.h"
            metadata_path = root / "test-warning-waivers.json"
            tests_root = root / "Tests"
            tests_root.mkdir()
            registry_path.write_text(registry_text, encoding="utf-8")
            metadata_path.write_text(metadata_text, encoding="utf-8")
            for relative, text in (sources or {}).items():
                source_path = root / relative
                source_path.parent.mkdir(parents=True, exist_ok=True)
                source_path.write_text(text, encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "--registry",
                    str(registry_path),
                    "--metadata",
                    str(metadata_path),
                    "--tests-root",
                    str(tests_root),
                    "--as-of",
                    AS_OF,
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )


class TestWarningPolicy(ValidatorHarness):
    def test_accepts_exactly_owned_future_waivers(self) -> None:
        result = self.run_validator(
            registry("Flaky_A", "Flaky_B"),
            metadata(waiver("Flaky_A"), waiver("Flaky_B", owner="networking")),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated 2 flaky waiver", result.stdout)

    def test_rejects_registry_pattern_without_metadata(self) -> None:
        result = self.run_validator(
            registry("Flaky_A", "Flaky_B"),
            metadata(waiver("Flaky_A")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing metadata", result.stderr)

    def test_rejects_metadata_pattern_without_registry_entry(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"),
            metadata(waiver("Flaky_A"), waiver("Stale_Entry")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not present in TestWarnings.h", result.stderr)

    def test_rejects_unowned_placeholder(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"),
            metadata(waiver("Flaky_A", owner="unassigned")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("owner", result.stderr)

    def test_rejects_expired_waiver_at_as_of_date(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"),
            metadata(waiver("Flaky_A", expires=AS_OF)),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expired", result.stderr)

    def test_rejects_duplicate_registry_patterns(self) -> None:
        result = self.run_validator(
            registry("Flaky_A", "Flaky_A"),
            metadata(waiver("Flaky_A")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate pattern", result.stderr)

    def test_rejects_extra_metadata_fields(self) -> None:
        entry = waiver("Flaky_A")
        entry["reason"] = "duplicated source-of-truth"
        result = self.run_validator(registry("Flaky_A"), metadata(entry))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly", result.stderr)


class TestAssertionWaiverPolicy(ValidatorHarness):
    def run_assertion_case(
        self, assertion_entries: list[dict[str, object]], sources: dict[str, str]
    ) -> subprocess.CompletedProcess[str]:
        return self.run_validator(
            registry("Flaky_A"), metadata_v2([waiver("Flaky_A")], assertion_entries), sources
        )

    def test_accepts_registered_owned_future_site(self) -> None:
        result = self.run_assertion_case([assertion_waiver()], {"Tests/TestLoad.cpp": LOAD_TEST_SOURCE})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated 1 EXPECT_WARN_ONLY site", result.stdout)

    def test_rejects_unregistered_site(self) -> None:
        result = self.run_assertion_case([], {"Tests/TestLoad.cpp": LOAD_TEST_SOURCE})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unregistered EXPECT_WARN_ONLY waiver at Tests/TestLoad.cpp::Load_Spikes (line 7)", result.stderr)

    def test_schema_v1_metadata_cannot_hide_a_site(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"), metadata(waiver("Flaky_A")), {"Tests/TestLoad.cpp": LOAD_TEST_SOURCE}
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unregistered EXPECT_WARN_ONLY waiver", result.stderr)

    def test_rejects_expired_site_waiver(self) -> None:
        result = self.run_assertion_case(
            [assertion_waiver(expires=AS_OF)], {"Tests/TestLoad.cpp": LOAD_TEST_SOURCE}
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("assertionWaivers[0] is expired", result.stderr)

    def test_rejects_ownerless_site_waiver(self) -> None:
        entry = assertion_waiver()
        del entry["owner"]
        result = self.run_assertion_case([entry], {"Tests/TestLoad.cpp": LOAD_TEST_SOURCE})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly file, test, sites, owner, and expires", result.stderr)

    def test_rejects_placeholder_owner_on_site_waiver(self) -> None:
        result = self.run_assertion_case(
            [assertion_waiver(owner="TBD")], {"Tests/TestLoad.cpp": LOAD_TEST_SOURCE}
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("owner must be a named owner", result.stderr)

    def test_rejects_second_site_added_to_waived_test(self) -> None:
        source = LOAD_TEST_SOURCE.replace(
            '    EXPECT_WARN_ONLY(spikes <= 30, "host scheduling pressure");\n',
            '    EXPECT_WARN_ONLY(spikes <= 30, "host scheduling pressure");\n'
            '    EXPECT_WARN_ONLY(spikes <= 10, "piggybacked waiver");\n',
        )
        result = self.run_assertion_case([assertion_waiver()], {"Tests/TestLoad.cpp": source})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("declares 1 site(s) but the test has 2", result.stderr)

    def test_rejects_waiver_for_a_different_test_in_same_file(self) -> None:
        source = LOAD_TEST_SOURCE + '\nTEST(Load_Other)\n{\n    EXPECT_WARN_ONLY(1 == 2, "unowned");\n}\n'
        result = self.run_assertion_case([assertion_waiver()], {"Tests/TestLoad.cpp": source})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Tests/TestLoad.cpp::Load_Other", result.stderr)

    def test_rejects_stale_site_waiver(self) -> None:
        result = self.run_assertion_case([assertion_waiver()], {})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stale assertion waiver", result.stderr)

    def test_rejects_site_outside_a_test_body(self) -> None:
        source = (
            '#include "TestFramework.h"\n\nstatic void Helper(int v)\n{\n'
            '    EXPECT_WARN_ONLY(v < 3, "hidden");\n}\n'
        )
        result = self.run_assertion_case([], {"Tests/Helpers/TestHelper.cpp": source})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Tests/Helpers/TestHelper.cpp:5: EXPECT_WARN_ONLY outside a TEST/TEST_F body", result.stderr)

    def test_keys_fixture_tests_by_fixture_and_name(self) -> None:
        source = 'TEST_F(LoadFixture, Spikes)\n{\n    EXPECT_WARN_ONLY(value < 3, "jitter");\n}\n'
        accepted = self.run_assertion_case(
            [assertion_waiver(test="LoadFixture.Spikes")], {"Tests/TestLoad.cpp": source}
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)

    def test_ignores_comments_and_string_literals(self) -> None:
        source = (
            "// EXPECT_WARN_ONLY(a, b) in a comment\n"
            "/* EXPECT_WARN_ONLY(a, b) */\n"
            'static const char* kText = "EXPECT_WARN_ONLY(a, b)";\n'
            'static const char* kRaw = R"x(EXPECT_WARN_ONLY(a, b))x";\n'
            "static const int kBig = 1'000;\n"
        )
        result = self.run_assertion_case([], {"Tests/TestText.cpp": source})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated 0 EXPECT_WARN_ONLY site", result.stdout)

    def test_exempts_only_runner_semantics_probes(self) -> None:
        probe = 'TEST(RunnerSemanticsReal_Probe)\n{\n    EXPECT_WARN_ONLY(1 == 2, "probe");\n}\n'
        accepted = self.run_assertion_case([], {"Tests/TestRunnerSemanticsReal.cpp": probe})
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("1 runner-semantics probe(s) exempt", accepted.stdout)

        smuggled = probe + 'TEST(Unrelated_Flaky)\n{\n    EXPECT_WARN_ONLY(1 == 2, "not a probe");\n}\n'
        rejected = self.run_assertion_case([], {"Tests/TestRunnerSemanticsReal.cpp": smuggled})
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("Tests/TestRunnerSemanticsReal.cpp::Unrelated_Flaky", rejected.stderr)

    def test_rejects_unknown_schema_version(self) -> None:
        text = json.dumps({"schemaVersion": 4, "waivers": [waiver("Flaky_A")], "assertionWaivers": []})
        result = self.run_validator(registry("Flaky_A"), text)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("schemaVersion must be 1, 2 or 3", result.stderr)


def metadata_v3(skip_reasons: list[dict[str, object]], dynamic_skips: list[dict[str, object]]) -> str:
    return (
        json.dumps(
            {
                "schemaVersion": 3,
                "waivers": [waiver("Flaky_A")],
                "assertionWaivers": [],
                "skipReasons": skip_reasons,
                "dynamicSkips": dynamic_skips,
            }
        )
        + "\n"
    )


def skip_reason(prefix: str, kind: str = "environment", owner: str = "rendering", **extra: str) -> dict[str, object]:
    return {"prefix": prefix, "owner": owner, "kind": kind, **extra}


def dynamic_skip(
    file: str = "Tests/TestSpawn.cpp", test: str | None = "Spawn_Peer", sites: int = 1
) -> dict[str, object]:
    return {"file": file, "test": test, "sites": sites, "owner": "persistence", "kind": "environment"}


DEVICE_SKIP_SOURCE = """#include "TestFramework.h"

TEST(Render_NeedsDevice)
{
    if (!HasDevice())
        SKIP_TEST("No D3D11 device available "
                  "(hardware or WARP)");
    EXPECT_TRUE(Draw());
}
"""

SPAWN_SKIP_SOURCE = """#include "TestFramework.h"

TEST(Spawn_Peer)
{
    auto launched = Spawn();
    if (!launched)
        SKIP_TEST("cannot spawn a peer: " + launched.error());
    EXPECT_TRUE(launched->Wait());
}
"""


class TestNoCrashAndSkipPolicy(ValidatorHarness):
    def run_case(
        self, sources: dict[str, str], skip_reasons=(), dynamic_skips=()
    ) -> subprocess.CompletedProcess[str]:
        return self.run_validator(registry("Flaky_A"), metadata_v3(list(skip_reasons), list(dynamic_skips)), sources)

    def test_rejects_no_crash_outside_the_runner_probes(self) -> None:
        source = 'TEST(Panel_SetterIsSafe)\n{\n    panel.Set(1);\n    EXPECT_NO_CRASH("no getter");\n}\n'
        result = self.run_case({"Tests/TestPanel.cpp": source})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("EXPECT_NO_CRASH at Tests/TestPanel.cpp:4 (Panel_SetterIsSafe)", result.stderr)

        helper = 'inline void Check()\n{\n    EXPECT_NO_CRASH("hidden in a helper");\n}\n'
        result = self.run_case({"Tests/PanelSupport.h": helper})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("EXPECT_NO_CRASH at Tests/PanelSupport.h:3 (outside a test body)", result.stderr)

    def test_accepts_no_crash_in_a_runner_probe_and_ignores_the_definition(self) -> None:
        probe = 'TEST(RunnerSemanticsReal_NoCrash)\n{\n    EXPECT_NO_CRASH("the probe");\n}\n'
        definition = "#define EXPECT_NO_CRASH(reason) static_cast<void>(reason)\n"
        result = self.run_case({"Tests/TestRunnerSemanticsReal.cpp": probe, "Tests/TestFramework.h": definition})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("outside the 1 runner-semantics probe(s)", result.stdout)

    def test_accepts_classified_literal_and_registered_dynamic_skips(self) -> None:
        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE, "Tests/TestSpawn.cpp": SPAWN_SKIP_SOURCE},
            [skip_reason("No D3D11 device available")],
            [dynamic_skip()],
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated 2 SKIP_TEST site(s)", result.stdout)
        self.assertIn("2 environment", result.stdout)

    def test_rejects_an_unclassified_skip_reason(self) -> None:
        result = self.run_case({"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "unclassified skip reason at Tests/TestRender.cpp:6: 'No D3D11 device available (hardware or WARP)'",
            result.stderr,
        )

    def test_rejects_an_ambiguously_classified_skip_reason(self) -> None:
        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE},
            [skip_reason("No D3D11"), skip_reason("No D3D11 device")],
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ambiguously classified skip reason", result.stderr)

    def test_rejects_a_flaky_skip_without_expiry(self) -> None:
        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE}, [skip_reason("No D3D11 device available", kind="flaky")]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("with kind 'flaky' must contain exactly expires, kind, owner, prefix", result.stderr)

    def test_rejects_an_expired_flaky_skip(self) -> None:
        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE},
            [skip_reason("No D3D11 device available", kind="flaky", expires=AS_OF)],
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("skipReasons[0] is expired", result.stderr)

    def test_rejects_an_environment_skip_with_an_expiry_or_without_an_owner(self) -> None:
        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE},
            [skip_reason("No D3D11 device available", expires="2027-01-01")],
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("with kind 'environment' must contain exactly kind, owner, prefix", result.stderr)

        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE}, [skip_reason("No D3D11 device available", owner="tbd")]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("owner must be a named owner", result.stderr)

    def test_rejects_an_unknown_skip_kind(self) -> None:
        result = self.run_case(
            {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE}, [skip_reason("No D3D11 device available", kind="later")]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kind must be one of environment, flaky, probe", result.stderr)

    def test_rejects_an_unregistered_or_miscounted_run_time_reason(self) -> None:
        result = self.run_case({"Tests/TestSpawn.cpp": SPAWN_SKIP_SOURCE})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unregistered run-time skip reason at Tests/TestSpawn.cpp::Spawn_Peer (line 7)", result.stderr)

        result = self.run_case({"Tests/TestSpawn.cpp": SPAWN_SKIP_SOURCE}, dynamic_skips=[dynamic_skip(sites=2)])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("declares 2 site(s)", result.stderr)

    def test_rejects_stale_skip_classifications(self) -> None:
        result = self.run_case({}, [skip_reason("No D3D11 device available")], [dynamic_skip()])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stale skip reason prefix", result.stderr)
        self.assertIn("stale dynamic skip entry", result.stderr)

    def test_forwarding_helper_is_classified_but_its_definition_is_not_a_site(self) -> None:
        helper = (
            "[[noreturn]] inline void SkipOrFail(const char* reason)\n{\n    SKIP_TEST(reason);\n}\n"
            'inline void Require()\n{\n    SkipOrFail("no Vulkan ICD available");\n}\n'
        )
        rejected = self.run_case({"Tests/VulkanSupport.h": helper}, [skip_reason("no Vulkan ICD available")])
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("unregistered run-time skip reason at Tests/VulkanSupport.h::outside a test body", rejected.stderr)

        accepted = self.run_case(
            {"Tests/VulkanSupport.h": helper},
            [skip_reason("no Vulkan ICD available")],
            [dynamic_skip(file="Tests/VulkanSupport.h", test=None)],
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("validated 2 SKIP_TEST site(s)", accepted.stdout)

    def test_schema_v2_metadata_cannot_hide_a_skip(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"), metadata_v2([waiver("Flaky_A")], []), {"Tests/TestRender.cpp": DEVICE_SKIP_SOURCE}
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unclassified skip reason", result.stderr)


if __name__ == "__main__":
    unittest.main()
