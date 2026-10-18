#!/usr/bin/env python3
"""In-memory negative controls for the installed runtime's syscall audit."""
import unittest
from unittest.mock import patch
from pathlib import Path

import asset_repository_isolation as isolation


class TraceAuditTests(unittest.TestCase):
    good = '123 openat(AT_FDCWD</result>, "/package/bin/Assets/Scenes/level1.scene", O_RDONLY) = 3</package/bin/Assets/Scenes/level1.scene>'

    def audit(self, line):
        return isolation.audit_trace([self.good, line], ["/checkout", "/build"])

    def test_package_asset_passes(self):
        self.assertEqual(self.audit(self.good), [])

    def test_denied_repository_lookup_fails_even_after_success(self):
        self.assertTrue(self.audit('123 openat(AT_FDCWD</result>, "/checkout/Assets/a.obj", O_RDONLY) = -1 ENOENT'))

    def test_relative_repository_asset_fails(self):
        self.assertTrue(self.audit('123 openat(AT_FDCWD</result>, "../checkout/Assets/a.obj", O_RDONLY) = -1 ENOENT'))

    def test_relative_directory_descriptor_resolves(self):
        self.assertEqual(self.audit('123 openat(7</package/bin/Assets>, "Models/a.obj", O_RDONLY) = 8</package/bin/Assets/Models/a.obj>'), [])

    def test_unannotated_relative_asset_fails(self):
        self.assertTrue(self.audit('123 stat("Assets/a.obj", {}) = -1 ENOENT'))

    def test_failed_package_lookups_do_not_count_as_reads(self):
        self.assertTrue(isolation.audit_trace(['123 stat("/package/bin/Assets/a.obj", {}) = -1 ENOENT'], []))

    def test_metadata_only_access_does_not_count_as_content(self):
        self.assertTrue(isolation.audit_trace(['123 stat("/package/bin/Assets/a.obj", {}) = 0'], []))

    def test_resolved_descriptor_escape_fails(self):
        self.assertTrue(self.audit('123 openat(AT_FDCWD</result>, "/package/bin/Assets/a.obj", O_RDONLY) = 3</checkout/Assets/a.obj>'))

    def test_empty_trace_fails(self):
        self.assertTrue(isolation.audit_trace([], []))

    def test_checkout_under_system_mount_is_rejected(self):
        with self.assertRaises(ValueError):
            isolation.sandbox_command(Path("/stage"), Path("/output"), ["/usr/src/engine"])

    def test_namespace_mounts_do_not_expose_checkout(self):
        with patch.object(isolation.shutil, "which", side_effect=lambda name: "/usr/bin/" + name):
            command = isolation.sandbox_command(Path("/stage"), Path("/output"), ["/checkout", "/build"])
        self.assertIn("--unshare-all", command)
        self.assertIn("--clearenv", command)
        self.assertNotIn("--ro-bind / /", " ".join(command))
        self.assertEqual(command[-2:], ["/checkout", "/build"])


if __name__ == "__main__":
    unittest.main()
