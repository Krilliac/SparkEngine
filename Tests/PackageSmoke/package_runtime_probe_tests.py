#!/usr/bin/env python3
"""Focused contract tests for package_runtime_probe.py."""

from __future__ import annotations

import argparse
from pathlib import Path
import unittest
from unittest import mock

import package_runtime_probe as probe


class PackageRuntimeProbeTests(unittest.TestCase):
    def test_runtime_result_is_bounded_and_does_not_claim_no_display(self) -> None:
        path = Path("package-smoke.log")
        args = argparse.Namespace(commit_sha="a" * 40, msi_sha256="b" * 64)
        with mock.patch.object(Path, "write_text") as write_text:
            probe.write_result(path, args, asset_entries=12)
            result = write_text.call_args.args[0]
            self.assertEqual(result.count("headlessNoDisplay"), 1)
            self.assertIn('"headlessNoDisplay": "unproven"', result)
            self.assertIn('"entries": 12', result)

    def test_layout_rejects_missing_scene(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "package root"):
            probe.validate_package_layout(Path("missing-package-root"))

    def test_integrity_requires_exactly_one_positive_terminal_count(self) -> None:
        args = argparse.Namespace(python=Path("python"), source_root=Path("source"))
        for output, success in (("OK: 12 entries verified (manifest v1)\n", True),
                                ("OK: 0 entries verified\n", False), ("", False),
                                ("OK: 12 entries verified\nOK: 13 entries verified\n", False)):
            with self.subTest(output=output), mock.patch.object(Path, "write_text"), \
                    mock.patch.object(probe, "run_command", side_effect=[
                        probe.CommandResult(0, output, ""),
                        probe.CommandResult(0, "OK: 8 references in 2 scene and material files resolve to listed staged assets\n", "")]):
                if success:
                    self.assertEqual(probe.verify_assets(args, Path("package"), Path("logs")), 12)
                else:
                    with self.assertRaises(RuntimeError):
                        probe.verify_assets(args, Path("package"), Path("logs"))

    def test_installed_reference_closure_must_run_and_pass(self) -> None:
        args = argparse.Namespace(python=Path("python"), source_root=Path("source"))
        for code, output in ((9, "failed"), (0, ""),
                             (0, "OK: 0 references in 0 scene and material files resolve to listed staged assets\n")):
            with self.subTest(code=code, output=output), mock.patch.object(Path, "write_text"), \
                    mock.patch.object(probe, "run_command", side_effect=[
                        probe.CommandResult(0, "OK: 12 entries verified\n", ""),
                        probe.CommandResult(code, output, "")]):
                with self.assertRaisesRegex(RuntimeError, "reference closure"):
                    probe.verify_assets(args, Path("package"), Path("logs"))

    def test_probe_process_failures_are_fatal(self) -> None:
        args = argparse.Namespace(cmake=Path("cmake"), source_root=Path("source"), build_root=Path("build"))
        for probe_call in (probe.run_authored_scene, probe.run_save_reload):
            with self.subTest(probe=probe_call.__name__), mock.patch.object(Path, "write_text"), \
                    mock.patch.object(probe, "run_command", return_value=probe.CommandResult(9, "", "failed")):
                with self.assertRaises(RuntimeError):
                    probe_call(args, Path("package"), Path("logs"))


if __name__ == "__main__":
    unittest.main()
