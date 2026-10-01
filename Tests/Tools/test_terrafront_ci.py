#!/usr/bin/env python3
"""TF-110 / NET-100: gate wiring and fail-closed production evidence contracts."""

from __future__ import annotations

import copy
import io
import json
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools" / "Terrafront"))
import check_selection
import multiclient as mc
import test_terrafront_multiclient as fixture


class SelectionTests(unittest.TestCase):
    def document(self, label="terrafront-multiclient"):
        return {"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": [
            {"name": name, "command": ["python", "multiclient.py"],
             "properties": [{"name": "LABELS", "value": [label]}]} for name in check_selection.EXPECTED[label]]}

    def test_exact_selections_pass(self):
        for label in check_selection.EXPECTED:
            with redirect_stdout(io.StringIO()):
                check_selection.check(self.document(label), label)

    def test_empty_missing_extra_duplicate_and_wrong_names_fail(self):
        good = self.document()
        for tests in ([], good["tests"][:-1], good["tests"] + [good["tests"][0]],
                      [{**test, "name": "unexpected"} for test in good["tests"]]):
            with self.assertRaises(ValueError):
                check_selection.check({**good, "tests": tests}, "terrafront-multiclient")

    def test_missing_label_or_executable_fails(self):
        for field in ("properties", "command"):
            document = self.document()
            document["tests"][0][field] = []
            with self.assertRaises(ValueError):
                check_selection.check(document, "terrafront-multiclient")

    def test_non_ctest_json_fails(self):
        with self.assertRaises(ValueError):
            check_selection.check({"tests": []}, "terrafront-multiclient")

    def test_workflow_is_advisory_but_steps_fail_closed(self):
        jobs = yaml.safe_load((ROOT / ".github/workflows/build.yml").read_text())["jobs"]
        job = jobs["terrafront-multiclient"]
        self.assertEqual(job["runs-on"], "ubuntu-24.04")
        self.assertIs(job["continue-on-error"], True)
        self.assertNotIn("terrafront-multiclient", jobs["required-ci-gate"]["needs"])
        steps = job["steps"]
        runs = "\n".join(step.get("run", "") for step in steps)
        for flag in ("-DBUILD_TESTS=ON", "-DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON",
                     "-DSPARK_HEADLESS_SUPPORT=ON", "--target SparkEngine SparkGameMMOFPS SparkTests"):
            self.assertIn(flag, runs)
        for label in check_selection.EXPECTED:
            self.assertIn(f"--show-only=json-v1 -L '^{label}$'", runs)
            self.assertIn(f"check_selection.py {label}-selection.json --label {label}", runs)
            run = next(step["run"] for step in steps if f"-L '^{label}$'" in step.get("run", "")
                       and "--show-only" not in step["run"])
            self.assertIn("--no-tests=error", run)
            self.assertIn("set -euo pipefail", run)
        for step in steps:
            self.assertNotIn("continue-on-error", step)
        upload = next(step for step in steps if "upload-artifact@" in step.get("uses", ""))
        self.assertEqual(upload["if"], "always()")
        self.assertIn("**/summary.json", upload["with"]["path"])
        self.assertIn("**/exec_audit.log", upload["with"]["path"])
        self.assertNotIn("**/userdata", upload["with"]["path"])

    def test_all_six_impaired_scenarios_are_registered_separately(self):
        cmake = (ROOT / "Tests/CMakeLists.txt").read_text()
        start = cmake.index("# Separate entries preserve bounded execution")
        block = cmake[start:cmake.index("endforeach()", start)]
        for suffix, name in zip(check_selection.SCENARIOS, mc.SCENARIOS):
            self.assertIn(f"{suffix}:{name}", block)
        self.assertIn("--timeout 150", block)
        self.assertIn("TIMEOUT 240", block)
        self.assertEqual(len(check_selection.EXPECTED["terrafront-multiclient"]), 15)


class EncryptionEvidenceTests(unittest.TestCase):
    def views(self):
        observed = []
        for i in range(3):
            line = fixture.net_line() + (f" sealedSent={10 + i} sealedReceived={20 + i}"
                                        " plaintextFramesDropped=0 unsealedSendsRefused=0")
            observed.append(mc.parse_observations(fixture.observation(
                "client", 2, fixture.TRUTH[i], net_view=line))[0])
        return mc.RunViews(copy.deepcopy(observed), [copy.deepcopy(observed), copy.deepcopy(observed)], [])

    def test_positive_traffic_and_movement_deltas_pass(self):
        problems, metrics = mc.encrypted_transport_verdict(self.views())
        self.assertEqual(problems, [])
        self.assertEqual(set(metrics), {"server", "client1", "client2"})

    def test_absent_old_zero_refused_and_nonadvancing_counters_fail(self):
        from dataclasses import replace
        for change in (None, {"sealed_sent": -1}, {"sealed_received": 0},
                       {"plaintext_dropped": 1}, {"unsealed_refused": 1}, {"sealed_sent": 10}):
            views = self.views()
            views.clients[1][1].net = (None if change is None else replace(views.clients[1][1].net, **change))
            problems, _ = mc.encrypted_transport_verdict(views)
            self.assertTrue(problems, change)

    def test_malformed_counter_invalidates_the_observation(self):
        lines = fixture.observation("client", 2, fixture.TRUTH[0], net_view=fixture.net_line() + " sealedSent=bad")
        self.assertEqual(mc.parse_observations(lines), [])


class SummaryTests(unittest.TestCase):
    def test_success_and_failure_both_persist_json(self):
        for passed in (True, False):
            summary = {"passed": passed, "problems": [] if passed else ["missing audit"], "checkpoints": []}
            with mock.patch.object(mc, "run_all", return_value=summary), \
                    mock.patch.object(mc.Path, "mkdir"), mock.patch.object(mc.Path, "write_text") as write, \
                    redirect_stdout(io.StringIO()), mock.patch.object(sys, "stderr", io.StringIO()):
                code = mc.main(["--engine", sys.executable, "--module", __file__, "--scenario", "onboard_spawn_move",
                                "--workdir", str(ROOT / "build/unused-mocked-run")])
            self.assertEqual(code, 0 if passed else 1)
            self.assertEqual(json.loads(write.call_args.args[0]), summary)


class FreshClientTests(unittest.TestCase):
    def logs(self):
        return (mc.RoleLog("client2", 0, 100.0, fixture.char_list_audit([41]), "auc"),
                mc.RoleLog("client2-reconnect", 0, 131.2, fixture.char_list_audit([41]), "auc"))

    def test_split_scripts_reauthenticate_without_registering_again(self):
        scenario = mc.SCENARIOS["reconnect"]
        creds = {"user": "testuser", "password": "testpassword", "name": "testname"}
        first = mc.client_script(scenario, 23000, "auc", 1, creds, end_seconds=mc.RECONNECT_EXIT_S)
        second = mc.client_script(scenario, 23000, "auc", 1, creds, start_seconds=mc.RECONNECT_START_S)
        self.assertEqual(first.count("tf_login testuser testpassword"), 1)
        self.assertEqual(second.count("tf_login testuser testpassword"), 1)
        self.assertIn("tf_disconnect", first)
        self.assertNotIn("tf_register", second)
        self.assertNotIn("tf_char_create", second)
        self.assertEqual(first.count("tf_observe"), 2)
        self.assertEqual(second.count("tf_observe"), 1)

    def test_join_uses_measured_restart_clock(self):
        first, second = self.logs()
        joined = mc.join_reconnected_client(first, second, None)
        self.assertAlmostEqual(joined.entries[-1].seconds, second.entries[-1].seconds + 31.2)
        self.assertEqual(joined.anchor, first.anchor)

    def test_crash_missing_log_or_changed_character_fails(self):
        for mutation in ("crash", "anchor", "empty", "character"):
            first, second = self.logs()
            if mutation == "crash":
                first.returncode = 1
            elif mutation == "anchor":
                second.anchor = None
            elif mutation == "empty":
                second.entries = []
            else:
                second.entries = fixture.char_list_audit([42])
            with self.assertRaises(mc.HarnessError):
                mc.join_reconnected_client(first, second, None)

    def test_each_process_must_prove_impairment(self):
        first, second = self.logs()
        with self.assertRaisesRegex(mc.HarnessError, "retain requested impairment"):
            mc.join_reconnected_client(first, second, fixture.IMPAIRMENT)

    def test_runner_launches_replacement_only_after_original_exit(self):
        first, second = self.logs()
        children = []
        waited = []
        def launch(role, args, workdir, script, seconds):
            if role == "client2-reconnect":
                self.assertIn("client2", waited)
            child = mock.Mock(role=role, anchor=100.0, process=mock.Mock(returncode=0, pid=100 + len(children)))
            child.log.return_value = (first if role == "client2" else second if role == "client2-reconnect"
                                      else mc.RoleLog(role, 0, 100.0, []))
            children.append(child)
            return child
        def wait(children, deadline, on_poll=None):
            waited.extend(child.role for child in children)
        args = mock.Mock(impairment=None, timeout=200)
        with mock.patch.object(mc, "launch", side_effect=launch), \
                mock.patch.object(mc, "wait_for_exit", side_effect=wait), \
                mock.patch.object(mc, "wait_for_server"), mock.patch.object(mc, "stop_all"), \
                mock.patch.object(mc, "free_udp_port", return_value=23000), \
                mock.patch.object(mc.Path, "mkdir"), mock.patch.object(mc.Path, "unlink"), \
                mock.patch.object(mc, "evaluate_views", return_value=(
                    {"passed": True, "problems": []}, mc.RunViews([], [], []))):
            summary = mc.run(args, "reconnect", ROOT / "build/unused-mocked-run")
        self.assertEqual([child.role for child in children], ["server", "client1", "client2", "client2-reconnect"])
        self.assertNotEqual(summary["client_restart"]["originalPid"], summary["client_restart"]["replacementPid"])


if __name__ == "__main__":
    unittest.main()
