#!/usr/bin/env python3
"""SEC-100 / OD-05 ratchet: no process switch or config key enables remote administration.

Remote administration is permanently unavailable in stable-v1. The runtime
tests prove SparkServer rejects the obvious spellings; this scan closes the
other direction by failing when any engine, server, gateway or editor source
starts *accepting* one: a command-line flag literal (``"--rcon"``,
``L"-remote-admin"``) or a ConfigParser key read (``GetString("Net",
"rcon_password")``) whose name is shaped like a remote-administration switch.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Every first-party surface that parses a process command line or reads an
# operator config file.
SCAN_ROOTS = (
    "SparkEngine/Source/Core",
    "SparkEngine/Source/Utils",
    "SparkServer/src",
    "SparkGateway/src",
    "SparkEditor/Source",
)
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".inl"}

# A flag literal: optional wide/UTF prefix, one or two dashes, a name.
FLAG_LITERAL = re.compile(r'(?:\bL|\bu8|\bu|\bU)?"(-{1,2}[A-Za-z][A-Za-z0-9_.-]*)(?:[=:][^"]*)?"')
# The key argument of a ConfigParser read: Get*/HasKey("Section", "key" ...).
CONFIG_KEY_READ = re.compile(
    r'\b(?:GetString|GetInt|GetFloat|GetBool|GetDouble|HasKey)\s*\(\s*"[^"]*"\s*,\s*"([^"]+)"'
)
REMOTE_ADMIN_NAME = re.compile(r"rcon|remote.?admin|remote.?debug|admin.?(?:port|password)", re.IGNORECASE)


def remote_admin_switches(source: str) -> list[str]:
    """Flag literals and config keys in ``source`` that name a remote-admin switch."""
    names = [match.group(1) for match in FLAG_LITERAL.finditer(source)]
    names += [match.group(1) for match in CONFIG_KEY_READ.finditer(source)]
    return [name for name in names if REMOTE_ADMIN_NAME.search(name)]


def scanned_sources() -> list[Path]:
    sources: list[Path] = []
    for root in SCAN_ROOTS:
        directory = REPO_ROOT / root
        if not directory.is_dir():
            raise FileNotFoundError(f"OD-05 scan root is missing: {root}")
        sources.extend(
            path
            for path in sorted(directory.rglob("*"))
            if path.suffix in SOURCE_SUFFIXES and path.is_file() and not path.is_symlink()
        )
    return sources


class RemoteAdminSwitchScanTests(unittest.TestCase):
    def test_repository_accepts_no_remote_admin_switch(self) -> None:
        sources = scanned_sources()
        # A scan that reads nothing passes vacuously; the roots hold hundreds
        # of sources, including the known argv parsers below.
        self.assertGreater(len(sources), 100)
        relative = {path.relative_to(REPO_ROOT).as_posix() for path in sources}
        for parser in ("SparkServer/src/ServerApplication.cpp", "SparkEngine/Source/Core/SparkEngineWindows.cpp"):
            self.assertIn(parser, relative)
        offenders = []
        for path in sources:
            text = path.read_text(encoding="utf-8", errors="replace")
            for name in remote_admin_switches(text):
                offenders.append(f"{path.relative_to(REPO_ROOT).as_posix()}: {name}")
        self.assertEqual([], offenders, "remote administration is unavailable in stable-v1 (OD-05)")

    def test_scanner_sees_the_real_argv_flags(self) -> None:
        # Guards the regex itself: if it stopped matching flag literals the
        # repository scan above would pass while checking nothing.
        server = (REPO_ROOT / "SparkServer/src/ServerApplication.cpp").read_text(encoding="utf-8")
        flags = {match.group(1) for match in FLAG_LITERAL.finditer(server)}
        self.assertTrue({"--bind-address", "--health-file", "--port"} <= flags, flags)
        keys = {match.group(1) for match in CONFIG_KEY_READ.finditer(server)}
        self.assertTrue({"bind_address", "port", "health_file"} <= keys, keys)

    def test_injected_switches_are_reported(self) -> None:
        injected = {
            'if (argument == "--rcon-password")': "--rcon-password",
            'else if (arg == L"-remote-admin")': "-remote-admin",
            'args.Has("--enable-remote-administration")': "--enable-remote-administration",
            'std::string_view flag = "--remote-debug=9999";': "--remote-debug",
            'config.GetInt("Network", "rcon_port", 27015)': "rcon_port",
            'config.HasKey("Server", "admin_password")': "admin_password",
            'cfg.GetBool( "Admin" , "RemoteAdmin" )': "RemoteAdmin",
        }
        for source, expected in injected.items():
            with self.subTest(source=source):
                self.assertEqual([expected], remote_admin_switches(source))

    def test_unrelated_flags_and_keys_pass(self) -> None:
        benign = (
            'if (argument == "--bind-address")',
            'config.GetString("Network", "bind_address")',
            'const char* text = "remote administration is unavailable";',
            'config.GetInt("Server", "max_admins", 1)',
        )
        for source in benign:
            with self.subTest(source=source):
                self.assertEqual([], remote_admin_switches(source))


if __name__ == "__main__":
    unittest.main()
